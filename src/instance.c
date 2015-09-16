/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <dlog.h>
#include <Eina.h>
#include <gio/gio.h>
#include <Ecore.h>

#include <packet.h>
#include <com-core_packet.h>
#include <widget_errno.h>
#include <widget_service.h>
#include <widget_service_internal.h>
#include <widget_cmd_list.h>
#include <widget_buffer.h>
#include <widget_conf.h>
#include <widget_util.h>

#include "conf.h"
#include "util.h"
#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "script_handler.h"
#include "buffer_handler.h"
#include "setting.h"
#include "monitor.h"

int errno;

static struct info {
	enum widget_fb_type env_buf_type;
} s_info = {
	.env_buf_type = WIDGET_FB_TYPE_FILE,
};

struct set_pinup_cbdata {
	struct inst_info *inst;
	int pinup;
};

struct resize_cbdata {
	struct inst_info *inst;
	int w;
	int h;
};

struct update_mode_cbdata {
	struct inst_info *inst;
	int active_update;
};

struct change_group_cbdata {
	struct inst_info *inst;
	char *cluster;
	char *category;
};

struct period_cbdata {
	struct inst_info *inst;
	double period;
};

struct event_item {
	int (*event_cb)(struct inst_info *inst, void *data);
	void *data;
	int deleted;
};

struct tag_item {
	char *tag;
	void *data;
};

struct inst_info {
	struct pkg_info *info;

	enum instance_state state; /*!< Represents current state */
	enum instance_state requested_state; /*!< Only ACTIVATED | DESTROYED is acceptable */
	widget_destroy_type_e destroy_type;
	int changing_state;
	int unicast_delete_event;

	char *id;
	double timestamp;

	char *content;
	char *cluster;
	char *category;
	char *title;
	int is_pinned_up;
	double sleep_at;
	int scroll_locked; /*!< Scroller which is in viewer is locked. */
	int active_update; /*!< Viewer will reload the buffer by itself, so the provider doesn't need to send the updated event */

	char *icon;
	char *name;

	enum widget_visible_state visible;

	struct {
		int need_to_recover;
	} watch;

	struct {
		int width;
		int height;
		double priority;

		union {
			struct script_info *script;
			struct buffer_info *buffer;
		} canvas;

		struct buffer_info **extra_buffer;

		double period;
	} widget;

	struct {
		int width;
		int height;
		double x;
		double y;

		union {
			struct script_info *script;
			struct buffer_info *buffer;
		} canvas;

		struct buffer_info **extra_buffer;

		struct client_node *owner;
		int is_opened_for_reactivate;
		int need_to_send_close_event;
		char *pended_update_desc;
		int pended_update_cnt;
	} gbar;

	struct client_node *client; /*!< Owner - creator */
	Eina_List *client_list; /*!< Viewer list */
	int refcnt;

	Ecore_Timer *update_timer; /*!< Only used for secured widget */
	int update_timer_freezed; /*!< Tizen 2.3 doesn't support ecore_timer_freeze_get API. it is introduced in Tizen 2.4. For the compatibility, master uses this */

	enum event_process {
		INST_EVENT_PROCESS_IDLE = 0x00,
		INST_EVENT_PROCESS_DELETE = 0x01
	} in_event_process;
	Eina_List *delete_event_list;

	Eina_List *data_list;
	int orientation;
};

static Eina_Bool update_timer_cb(void *data);

static int client_send_event(struct inst_info *instance, struct packet *packet, struct packet *owner_packet)
{
	/*!
	 * \note
	 * If a instance has owner, send event to it first.
	 */
	if (instance->client) {
		/*
		 * To prevent from packet destruction
		 */
		if (owner_packet) {
			client_rpc_async_request(instance->client, owner_packet);
		} else if (packet) {
			packet_ref(packet);
			client_rpc_async_request(instance->client, packet);
		}
	}

	/*!
	 * And then broadcast events to all client (viewer)
	 */
	return client_broadcast(instance, packet);
}

static inline void timer_thaw(struct inst_info *inst)
{
	double pending;
	double period;
	double delay;
	double sleep_time;

	if (!inst->update_timer_freezed) {
		return;
	}

	ecore_timer_thaw(inst->update_timer);
	inst->update_timer_freezed = 0;

	period = ecore_timer_interval_get(inst->update_timer);
	pending = ecore_timer_pending_get(inst->update_timer);
	delay = util_time_delay_for_compensation(period) - pending;
	ecore_timer_delay(inst->update_timer, delay);

	if (inst->sleep_at == 0.0f) {
		return;
	}

	sleep_time = util_timestamp() - inst->sleep_at;
	if (sleep_time > pending) {
		(void)update_timer_cb(inst);
	}

	inst->sleep_at = 0.0f;
}

static inline void timer_freeze(struct inst_info *inst)
{
	if (inst->update_timer_freezed) {
		return;
	}

	ecore_timer_freeze(inst->update_timer);
	inst->update_timer_freezed = 1;

	if (ecore_timer_interval_get(inst->update_timer) <= 1.0f) {
		return;
	}

#if defined(_USE_ECORE_TIME_GET)
	inst->sleep_at = ecore_time_get();
#else
	struct timeval tv;
	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("gettimeofday: %d\n", errno);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}
	inst->sleep_at = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
#endif
}

static int viewer_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	unsigned int cmd = CMD_VIEWER_DISCONNECTED;
	struct packet *packet;

	DbgPrint("%d is deleted from the list of viewer of %s(%s)\n", client_pid(client), package_name(instance_package(inst)), instance_id(inst));
	if (!eina_list_data_find(inst->client_list, client)) {
		ErrPrint("Not found\n");
		return WIDGET_ERROR_NOT_EXIST;
	}

	packet = packet_create_noack((const char *)&cmd, "sss", package_name(inst->info), inst->id, client_direct_addr(inst->client));
	if (packet) {
		if (slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0) < 0) {
			ErrPrint("Failed to send request to slave: %s\n", package_name(inst->info));
			/*!
			 * \note
			 * From now, packet is not valid. because the slave_rpc_request_only will destroy it if it fails;)
			 */
		}
	} else {
		ErrPrint("Failed to create a packet\n");
	}

	inst->client_list = eina_list_remove(inst->client_list, client);
	if (!inst->client_list && !inst->client) {
		DbgPrint("Has no clients\n");
		instance_destroy(inst, WIDGET_DESTROY_TYPE_FAULT);
	}

	instance_unref(inst);
	return -1; /*!< Remove this callback from the cb list */
}

static int pause_widget(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_WIDGET_PAUSE;

	packet = packet_create_noack((const char *)&cmd, "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

/*! \TODO Wake up the freeze'd timer */
static int resume_widget(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_WIDGET_RESUME;

	packet = packet_create_noack((const char *)&cmd, "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

static inline int instance_recover_visible_state(struct inst_info *inst)
{
	int ret;

	switch (inst->visible) {
	case WIDGET_SHOW:
	case WIDGET_HIDE:
		instance_thaw_updator(inst);

		ret = 0;
		break;
	case WIDGET_HIDE_WITH_PAUSE:
		ret = pause_widget(inst);
		(void)instance_freeze_updator(inst);

		break;
	default:
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		break;
	}

	DbgPrint("Visible state is recovered to %d\n", ret);
	return ret;
}

static inline void instance_send_update_mode_event(struct inst_info *inst, int active_mode, int status)
{
	struct packet *packet;
	const char *pkgname;
	unsigned int cmd = CMD_RESULT_UPDATE_MODE;

	if (!inst->info) {
		ErrPrint("Instance info is not ready to use\n");
		return;
	}

	pkgname = package_name(inst->info);

	packet = packet_create_noack((const char *)&cmd, "ssii", pkgname, inst->id, status, active_mode);
	if (packet) {
		client_send_event(inst, packet, NULL);
	} else {
		ErrPrint("Failed to send update mode event\n");
	}
}

static inline void instance_send_update_id(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_UPDATE_ID;

	packet = packet_create_noack((const char *)&cmd, "ds", inst->timestamp, inst->id);
	if (packet) {
		client_send_event(inst, packet, NULL);
	} else {
		ErrPrint("Failed to create update_id packet\n");
	}
}

static inline void instance_send_resized_event(struct inst_info *inst, int is_gbar, int w, int h, int status)
{
	struct packet *packet;
	enum widget_widget_type widget_type;
	const char *pkgname;
	const char *id;
	unsigned int cmd = CMD_SIZE_CHANGED;

	if (!inst->info) {
		ErrPrint("Instance info is not ready to use\n");
		return;
	}

	pkgname = package_name(inst->info);

	widget_type = package_widget_type(inst->info);
	if (widget_type == WIDGET_TYPE_SCRIPT) {
		id = script_handler_buffer_id(inst->widget.canvas.script);
	} else if (widget_type == WIDGET_TYPE_BUFFER) {
		id = buffer_handler_id(inst->widget.canvas.buffer);
	} else {
		id = "";
	}

	packet = packet_create_noack((const char *)&cmd, "sssiiii", pkgname, inst->id, id, is_gbar, w, h, status);
	if (packet) {
		client_send_event(inst, packet, NULL);
	} else {
		ErrPrint("Failed to send size changed event\n");
	}
}

static void update_mode_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct update_mode_cbdata *cbdata = data;
	int ret;

	if (!packet) {
		ErrPrint("Invalid packet\n");
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameters\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (ret == (int)WIDGET_ERROR_NONE) {
		cbdata->inst->active_update = cbdata->active_update;
	}

out:
	instance_send_update_mode_event(cbdata->inst, cbdata->active_update, ret);
	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_unicast_created_event(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;
	enum widget_widget_type widget_type;
	enum widget_gbar_type gbar_type;
	const char *widget_file;
	const char *gbar_file;
	unsigned int cmd = CMD_CREATED;

	if (!client) {
		client = inst->client;
		if (!client) {
			return WIDGET_ERROR_NONE;
		}
	}

	widget_type = package_widget_type(inst->info);
	gbar_type = package_gbar_type(inst->info);

	if (widget_type == WIDGET_TYPE_SCRIPT) {
		widget_file = script_handler_buffer_id(inst->widget.canvas.script);
	} else if (widget_type == WIDGET_TYPE_BUFFER) {
		widget_file = buffer_handler_id(inst->widget.canvas.buffer);
	} else {
		widget_file = "";
	}

	if (gbar_type == GBAR_TYPE_SCRIPT) {
		gbar_file = script_handler_buffer_id(inst->gbar.canvas.script);
	} else if (gbar_type == GBAR_TYPE_BUFFER) {
		gbar_file = buffer_handler_id(inst->gbar.canvas.buffer);
	} else {
		gbar_file = "";
	}

	packet = packet_create_noack((const char *)&cmd, "dsssiiiisssssdiiiiidsi",
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->widget.width, inst->widget.height,
			inst->gbar.width, inst->gbar.height,
			inst->cluster, inst->category,
			widget_file, gbar_file,
			package_auto_launch(inst->info),
			inst->widget.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			widget_type, gbar_type,
			inst->widget.period, inst->title,
			inst->is_pinned_up);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	return client_rpc_async_request(client, packet);
}

static int update_client_list(struct client_node *client, void *data)
{
	struct inst_info *inst = data;

	if (!instance_has_client(inst, client)) {
		instance_add_client(inst, client);
	}

	return WIDGET_ERROR_NONE;
}

static int instance_broadcast_created_event(struct inst_info *inst)
{
	struct packet *packet;
	struct packet *owner_packet;
	enum widget_widget_type widget_type;
	enum widget_gbar_type gbar_type;
	const char *widget_file;
	const char *gbar_file;
	unsigned int cmd = CMD_CREATED;
	int ret;

	widget_type = package_widget_type(inst->info);
	gbar_type = package_gbar_type(inst->info);

	if (widget_type == WIDGET_TYPE_SCRIPT) {
		widget_file = script_handler_buffer_id(inst->widget.canvas.script);
	} else if (widget_type == WIDGET_TYPE_BUFFER) {
		widget_file = buffer_handler_id(inst->widget.canvas.buffer);
	} else {
		widget_file = "";
	}

	if (gbar_type == GBAR_TYPE_SCRIPT) {
		gbar_file = script_handler_buffer_id(inst->gbar.canvas.script);
	} else if (gbar_type == GBAR_TYPE_BUFFER) {
		gbar_file = buffer_handler_id(inst->gbar.canvas.buffer);
	} else {
		gbar_file = "";
	}

	if (!inst->client) {
		client_browse_group_list(inst->cluster, inst->category, update_client_list, inst);
	}

	client_browse_category_list(package_category(inst->info), update_client_list, inst);
	inst->unicast_delete_event = 0;

	packet = packet_create_noack((const char *)&cmd, "dsssiiiisssssdiiiiidsi", 
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->widget.width, inst->widget.height,
			inst->gbar.width, inst->gbar.height,
			inst->cluster, inst->category,
			widget_file, gbar_file,
			package_auto_launch(inst->info),
			inst->widget.priority,
			package_size_list(inst->info),
			0, /* Assume, this is a system created widget (not the ower) */
			package_pinup(inst->info),
			widget_type, gbar_type,
			inst->widget.period, inst->title,
			inst->is_pinned_up);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	if (inst->client) {
		owner_packet = packet_create_noack((const char *)&cmd, "dsssiiiisssssdiiiiidsi", 
				inst->timestamp,
				package_name(inst->info), inst->id, inst->content,
				inst->widget.width, inst->widget.height,
				inst->gbar.width, inst->gbar.height,
				inst->cluster, inst->category,
				widget_file, gbar_file,
				package_auto_launch(inst->info),
				inst->widget.priority,
				package_size_list(inst->info),
				1,
				package_pinup(inst->info),
				widget_type, gbar_type,
				inst->widget.period, inst->title,
				inst->is_pinned_up);
		if (!owner_packet) {
			ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
			return WIDGET_ERROR_FAULT;
		}
	} else {
		owner_packet = NULL;
	}

	ret = client_send_event(inst, packet, owner_packet);

	monitor_multicast_state_change_event(package_name(inst->info), MONITOR_EVENT_CREATED, instance_id(inst), instance_content(inst));

	return ret;
}

HAPI int instance_unicast_deleted_event(struct inst_info *inst, struct client_node *client, int reason)
{
	struct packet *packet;
	unsigned int cmd = CMD_DELETED;

	if (!client) {
		client = inst->client;
		if (!client) {
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
	}

	packet = packet_create_noack((const char *)&cmd, "ssdi", package_name(inst->info), inst->id, inst->timestamp, reason);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	return client_rpc_async_request(client, packet);
}

static int instance_broadcast_deleted_event(struct inst_info *inst, int reason)
{
	struct packet *packet;
	struct client_node *client;
	Eina_List *l;
	Eina_List *n;
	int ret;
	unsigned int cmd = CMD_DELETED;

	packet = packet_create_noack((const char *)&cmd, "ssdi", package_name(inst->info), inst->id, inst->timestamp, reason);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	ret = client_send_event(inst, packet, NULL);

	EINA_LIST_FOREACH_SAFE(inst->client_list, l, n, client) {
		instance_del_client(inst, client);
	}

	monitor_multicast_state_change_event(package_name(inst->info), MONITOR_EVENT_DESTROYED, instance_id(inst), instance_content(inst));

	return ret;
}

static int client_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	instance_destroy(inst, WIDGET_DESTROY_TYPE_FAULT);
	return WIDGET_ERROR_NONE;
}

static int send_gbar_destroyed_to_client(struct inst_info *inst, int status)
{
	struct packet *packet;
	unsigned int cmd = CMD_GBAR_DESTROYED;

	if (!inst->gbar.need_to_send_close_event && status != WIDGET_ERROR_FAULT) {
		ErrPrint("GBAR is not created\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssi", package_name(inst->info), inst->id, status);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	inst->gbar.need_to_send_close_event = 0;

	return client_send_event(inst, packet, NULL);
}

static inline void invoke_delete_callbacks(struct inst_info *inst)
{
	Eina_List *l;
	Eina_List *n;
	struct event_item *item;

	inst->in_event_process |= INST_EVENT_PROCESS_DELETE;
	EINA_LIST_FOREACH_SAFE(inst->delete_event_list, l, n, item) {
		if (item->deleted || item->event_cb(inst, item->data) < 0 || item->deleted) {
			inst->delete_event_list = eina_list_remove(inst->delete_event_list, item);
			DbgFree(item);
		}
	}
	inst->in_event_process &= ~INST_EVENT_PROCESS_DELETE;
}

HAPI int instance_event_callback_is_added(struct inst_info *inst, enum instance_event type, int (*event_cb)(struct inst_info *inst, void *data), void *data)
{
	struct event_item *item;
	Eina_List *l;

	if (!event_cb) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (type) {
	case INSTANCE_EVENT_DESTROY:
		EINA_LIST_FOREACH(inst->delete_event_list, l, item) {
			if (item->event_cb == event_cb && item->data == data) {
				return 1;
			}
		}

		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	return 0;
}

HAPI int instance_event_callback_add(struct inst_info *inst, enum instance_event type, int (*event_cb)(struct inst_info *inst, void *data), void *data)
{
	struct event_item *item;

	if (!event_cb) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (type) {
	case INSTANCE_EVENT_DESTROY:
		item = malloc(sizeof(*item));
		if (!item) {
			ErrPrint("malloc: %d\n", errno);
			return WIDGET_ERROR_OUT_OF_MEMORY;
		}

		item->event_cb = event_cb;
		item->data = data;
		item->deleted = 0;

		inst->delete_event_list = eina_list_append(inst->delete_event_list, item);
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	return WIDGET_ERROR_NONE;
}

HAPI int instance_event_callback_del(struct inst_info *inst, enum instance_event type, int (*event_cb)(struct inst_info *inst, void *data), void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct event_item *item;

	switch (type) {
	case INSTANCE_EVENT_DESTROY:
		EINA_LIST_FOREACH_SAFE(inst->delete_event_list, l, n, item) {
			if (item->event_cb == event_cb && item->data == data) {
				if (inst->in_event_process & INST_EVENT_PROCESS_DELETE) {
					item->deleted = 1;
				} else {
					inst->delete_event_list = eina_list_remove(inst->delete_event_list, item);
					DbgFree(item);
				}
				return WIDGET_ERROR_NONE;
			}
		}
		break;
	default:
		break;
	}

	return WIDGET_ERROR_NOT_EXIST;
}

static inline void destroy_instance(struct inst_info *inst)
{
	struct pkg_info *pkg;
	enum widget_widget_type widget_type;
	enum widget_gbar_type gbar_type;
	struct slave_node *slave;
	struct event_item *item;
	struct tag_item *tag_item;

	(void)send_gbar_destroyed_to_client(inst, WIDGET_ERROR_NONE);

	invoke_delete_callbacks(inst);

	pkg = inst->info;

	widget_type = package_widget_type(pkg);
	gbar_type = package_gbar_type(pkg);
	slave = package_slave(inst->info);

	DbgPrint("Instance is destroyed (%p), slave(%p)\n", inst, slave);

	if (widget_type == WIDGET_TYPE_SCRIPT) {
		(void)script_handler_unload(inst->widget.canvas.script, 0);
		if (script_handler_destroy(inst->widget.canvas.script) == (int)WIDGET_ERROR_NONE) {
			inst->widget.canvas.script = NULL;
		}
	} else if (widget_type == WIDGET_TYPE_BUFFER) {
		(void)buffer_handler_unload(inst->widget.canvas.buffer);
		if (buffer_handler_destroy(inst->widget.canvas.buffer) == (int)WIDGET_ERROR_NONE) {
			inst->widget.canvas.buffer = NULL;
		}

		if (inst->widget.extra_buffer) {
			int i;

			for (i = 0; i < WIDGET_CONF_EXTRA_BUFFER_COUNT; i++) {
				if (!inst->widget.extra_buffer[i]) {
					continue;
				}

				(void)buffer_handler_unload(inst->widget.extra_buffer[i]);
				if (buffer_handler_destroy(inst->widget.extra_buffer[i]) == (int)WIDGET_ERROR_NONE) {
					inst->widget.extra_buffer[i] = NULL;
				}
			}

			DbgFree(inst->widget.extra_buffer);
			inst->widget.extra_buffer = NULL;
		}
	}

	if (gbar_type == GBAR_TYPE_SCRIPT) {
		(void)script_handler_unload(inst->gbar.canvas.script, 1);
		if (script_handler_destroy(inst->gbar.canvas.script) == (int)WIDGET_ERROR_NONE) {
			inst->gbar.canvas.script = NULL;
		}
	} else if (gbar_type == GBAR_TYPE_BUFFER) {
		(void)buffer_handler_unload(inst->gbar.canvas.buffer);
		if (buffer_handler_destroy(inst->gbar.canvas.buffer) == (int)WIDGET_ERROR_NONE) {
			inst->gbar.canvas.buffer = NULL;
		}
		if (inst->gbar.extra_buffer) {
			int i;

			for (i = 0; i < WIDGET_CONF_EXTRA_BUFFER_COUNT; i++) {
				if (!inst->gbar.extra_buffer[i]) {
					continue;
				}

				(void)buffer_handler_unload(inst->gbar.extra_buffer[i]);
				if (buffer_handler_destroy(inst->gbar.extra_buffer[i]) == (int)WIDGET_ERROR_NONE) {
					inst->gbar.extra_buffer[i] = NULL;
				}
			}

			DbgFree(inst->gbar.extra_buffer);
			inst->gbar.extra_buffer = NULL;
		}
	}

	if (inst->client) {
		client_event_callback_del(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst);
		client_unref(inst->client);
	}

	if (inst->update_timer) {
		ecore_timer_del(inst->update_timer);
	}

	EINA_LIST_FREE(inst->data_list, tag_item) {
		DbgPrint("Tagged item[%s] %p\n", tag_item->tag, tag_item->data);
		DbgFree(tag_item->tag);
		DbgFree(tag_item);
	}

	EINA_LIST_FREE(inst->delete_event_list, item) {
		DbgFree(item);
	}

	DbgFree(inst->icon);
	DbgFree(inst->name);
	DbgFree(inst->category);
	DbgFree(inst->cluster);
	DbgFree(inst->content);
	DbgFree(inst->title);
	util_unlink(widget_util_uri_to_path(inst->id));
	DbgFree(inst->id);
	package_del_instance(inst->info, inst);
	DbgFree(inst);

	slave = slave_unload_instance(slave);
}

static Eina_Bool update_timer_cb(void *data)
{
	struct inst_info *inst = (struct inst_info *)data;

	slave_rpc_request_update(package_name(inst->info), inst->id, inst->cluster, inst->category, NULL, 0);
	return ECORE_CALLBACK_RENEW;
}

static inline void unfork_package(struct inst_info *inst)
{
	DbgFree(inst->id);
	inst->id = NULL;

	if (inst->update_timer) {
		ecore_timer_del(inst->update_timer);
		inst->update_timer = NULL;
	}
}

static inline int fork_package(struct inst_info *inst, const char *pkgname)
{
	struct pkg_info *info;
	int len;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("%s is not found\n", pkgname);
		return WIDGET_ERROR_NOT_EXIST;
	}

	len = strlen(SCHEMA_FILE "%s%s_%d_%lf.png") + strlen(WIDGET_CONF_IMAGE_PATH) + strlen(package_name(info)) + 50;
	inst->id = malloc(len);
	if (!inst->id) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	snprintf(inst->id, len, SCHEMA_FILE "%s%s_%d_%lf.png", WIDGET_CONF_IMAGE_PATH, package_name(info), client_pid(inst->client), inst->timestamp);

	instance_set_gbar_size(inst, package_gbar_width(info), package_gbar_height(info));

	inst->widget.period = package_period(info);

	inst->info = info;

	if (package_secured(info) || (WIDGET_IS_INHOUSE(package_abi(info)) && WIDGET_CONF_SLAVE_LIMIT_TO_TTL)) {
		if (inst->widget.period > 0.0f) {
			inst->update_timer = util_timer_add(inst->widget.period, update_timer_cb, inst);
			(void)instance_freeze_updator(inst);
		} else {
			inst->update_timer = NULL;
		}
	}

	return WIDGET_ERROR_NONE;
}

HAPI struct inst_info *instance_create(struct client_node *client, double timestamp, const char *pkgname, const char *content, const char *cluster, const char *category, double period, int width, int height)
{
	struct inst_info *inst;
	char *extra_bundle_data = NULL;

	inst = calloc(1, sizeof(*inst));
	if (!inst) {
		ErrPrint("calloc: %d\n", errno);
		return NULL;
	}

	inst->timestamp = timestamp;
	inst->widget.width = width;
	inst->widget.height = height;

	if (client_is_sdk_viewer(client)) {
		char *tmp;

		tmp = strdup(content);
		if (tmp) {
			int length;
			char *ptr = tmp;

			if (sscanf(content, "%d:%s", &length, ptr) == 2) {
				extra_bundle_data = malloc(length + 1);
				if (extra_bundle_data) {
					strncpy(extra_bundle_data, tmp, length);
					extra_bundle_data[length] = '\0';
					ptr += length;
					DbgPrint("Extra Bundle Data extracted: [%s]\n", extra_bundle_data);
				}

				if (*ptr) {
					inst->content = strdup(ptr);
					if (!inst->content) {
						ErrPrint("strdup: %d\n", errno);
					}
					DbgPrint("Content Info extracted: [%d]\n", inst->content);
				}
			}

			DbgFree(tmp);
		}
	} else {
		inst->content = strdup(content);
		if (!inst->content) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(extra_bundle_data);
			DbgFree(inst);
			return NULL;
		}
	}

	inst->cluster = strdup(cluster);
	if (!inst->cluster) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(extra_bundle_data);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	inst->category = strdup(category);
	if (!inst->category) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(extra_bundle_data);
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	inst->title = strdup(WIDGET_CONF_DEFAULT_TITLE); /*!< Use the DEFAULT Title "" */
	if (!inst->title) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(extra_bundle_data);
		DbgFree(inst->category);
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	if (client) {
		inst->client = client_ref(client);
		if (client_event_callback_add(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst) < 0) {
			ErrPrint("Failed to add client event callback: %s\n", inst->id);
		}
		inst->orientation = client_orientation(client);
	}

	if (fork_package(inst, pkgname) < 0) {
		(void)client_unref(inst->client);
		DbgFree(extra_bundle_data);
		DbgFree(inst->title);
		DbgFree(inst->category);
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	inst->unicast_delete_event = 1;
	inst->state = INST_INIT;
	inst->requested_state = INST_INIT;
	if (WIDGET_CONF_EXTRA_BUFFER_COUNT) {
		inst->widget.extra_buffer = calloc(WIDGET_CONF_EXTRA_BUFFER_COUNT, sizeof(*inst->widget.extra_buffer));
		if (!inst->widget.extra_buffer) {
			ErrPrint("Failed to allocate buffer for widget extra buffer\n");
		}

		inst->gbar.extra_buffer = calloc(WIDGET_CONF_EXTRA_BUFFER_COUNT, sizeof(*inst->gbar.extra_buffer));
		if (!inst->gbar.extra_buffer) {
			ErrPrint("Failed to allocate buffer for gbar extra buffer\n");
		}
	}
	instance_ref(inst);

	if (package_add_instance(inst->info, inst) < 0) {
		DbgFree(extra_bundle_data);
		DbgFree(inst->widget.extra_buffer);
		DbgFree(inst->gbar.extra_buffer);
		unfork_package(inst);
		(void)client_unref(inst->client);
		DbgFree(inst->title);
		DbgFree(inst->category);
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	slave_load_instance(package_slave(inst->info));
	slave_set_extra_bundle_data(package_slave(inst->info), extra_bundle_data);
	DbgFree(extra_bundle_data);

	/**
	 * @note
	 * Before activate an instance,
	 * Update its Id first for client.
	 */
	instance_send_update_id(inst);

	if (instance_activate(inst) < 0) {
		instance_state_reset(inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_FAULT);
		inst = NULL;
	} else {
		inst->visible = WIDGET_HIDE_WITH_PAUSE;
	}

	return inst;
}

HAPI struct packet *instance_duplicate_packet_create(const struct packet *packet, struct inst_info *inst, int width, int height)
{
	struct packet *result;

	/**
	 * Do not touch the "timestamp".
	 * It is assigned by Viewer, and will be used as a key to find this instance
	 */
	instance_set_widget_size(inst, width, height);

	/**
	 * @TODO
	 */
	DbgPrint("[TODO] Instance package info: %p:%s\n", inst->info, package_name(inst->info));
	// inst->info = info;

	inst->unicast_delete_event = 1;
	result = packet_create_reply(packet, "sssiidssisiis",
			package_name(inst->info),
			inst->id,
			inst->content,
			package_timeout(inst->info),
			!!package_widget_path(inst->info),
			inst->widget.period,
			inst->cluster,
			inst->category,
			!!inst->client,
			package_abi(inst->info),
			inst->widget.width,
			inst->widget.height,
			client_direct_addr(inst->client));
	if (!result) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return NULL;
	}

	/**
	 * @note
	 * Is this really necessary?
	inst->requested_state = INST_ACTIVATED;
	 */
	DbgPrint("[TODO] Instance request_state is not touched\n");
	inst->changing_state--;

	inst->state = INST_ACTIVATED;

	if (inst->requested_state == INST_DESTROYED) {
		/**
		 * In this case, we should destroy the instance.
		 */
		DbgPrint("Destroy Instance\n");
		instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
	}

	instance_create_widget_buffer(inst, WIDGET_CONF_DEFAULT_PIXELS);
	instance_broadcast_created_event(inst);
	instance_thaw_updator(inst);

	return result;
}

HAPI struct inst_info *instance_ref(struct inst_info *inst)
{
	if (!inst) {
		return NULL;
	}

	inst->refcnt++;
	return inst;
}

HAPI struct inst_info *instance_unref(struct inst_info *inst)
{
	if (!inst) {
		return NULL;
	}

	if (inst->refcnt == 0) {
		ErrPrint("Instance refcnt is not valid\n");
		return NULL;
	}

	inst->refcnt--;
	if (inst->refcnt == 0) {
		destroy_instance(inst);
		inst = NULL;
	}

	return inst;
}

static void deactivate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	int ret;

	/*!
	 * \note
	 * In this callback, we cannot trust the "client" information.
	 * It could be cleared before reach to here.
	 */

	if (!packet) {
		/*!
		 * \note
		 * The instance_reload will care this.
		 * And it will be called from the slave activate callback.
		 */
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	DbgPrint("[%s] %d (0x%X)\n", inst->id, ret, inst->state);

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		goto out;
	}

	switch (ret) {
	case 0:
		/*!
		 * \note
		 * Successfully unloaded
		 */
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			/**
			 * @todo
			 * How about watch? is it possible to be jumped into this case??
			 */
			instance_state_reset(inst);
			instance_reactivate(inst);
			break;
		case INST_DESTROYED:
			if (slave_is_watch(slave)) {
				/**
				 * @note
				 * In case of the watch app.
				 * It will be terminated by itself if master send the instance destroy request.
				 * So the master should not handles provider termination as a fault.
				 * To do that, change the state of a provider too from here.
				 * Only in case of the slave is watch app.
				 *
				 * Watch App will returns DESTROYED result to the master before terminate its process.
				 * So we can change the state of slave from here.
				 * The master will not change the states of the slave as a faulted one.
				 */
				DbgPrint("Change the slave state for Watch app\n");
				slave_set_state(slave, SLAVE_REQUEST_TO_TERMINATE);
			}

			if (inst->unicast_delete_event) {
				instance_unicast_deleted_event(inst, NULL, ret);
			} else {
				instance_broadcast_deleted_event(inst, ret);
			}

			instance_state_reset(inst);
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
		default:
			/*!< Unable to reach here */
			break;
		}

		break;
	case WIDGET_ERROR_INVALID_PARAMETER:
		/**
		 * @note
		 * Slave has no instance of this package.
		 */
	case WIDGET_ERROR_NOT_EXIST:
		/**
		 * @note
		 * This instance's previous state is only can be the INST_ACTIVATED.
		 * So we should care the slave_unload_instance from here.
		 * And we should send notification to clients, about this is deleted.
		 */
		/**
		 * @note
		 * Slave has no instance of this.
		 * In this case, ignore the requested_state
		 * Because, this instance is already met a problem.
		 */
	default:
		/**
		 * @note
		 * Failed to unload this instance.
		 * This is not possible, slave will always return WIDGET_ERROR_NOT_EXIST, WIDGET_ERROR_INVALID_PARAMETER, or 0.
		 * but care this exceptional case.
		 */

		/**
		 * @todo
		 * How about watch? is it possible to be jumped into this case??
		 */
		instance_broadcast_deleted_event(inst, ret);
		instance_state_reset(inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
		break;
	}

out:
	inst->changing_state--;
	if (inst->changing_state < 0) {
		ErrPrint("Changing state is not correct: %d\n", inst->changing_state);
	}
	instance_unref(inst);
}

static void reactivate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	enum widget_widget_type widget_type;
	enum widget_gbar_type gbar_type;
	int ret;
	const char *content;
	const char *title;
	int is_pinned_up;

	if (!packet) {
		/*!
		 * \note
		 * instance_reload function will care this.
		 * and it will be called from the slave_activate callback
		 */
		goto out;
	}

	if (packet_get(packet, "issi", &ret, &content, &title, &is_pinned_up) != 4) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (strlen(content)) {
		char *tmp;

		tmp = strdup(content);
		if (!tmp) {
			ErrPrint("strdup: %d\n", errno);
			goto out;
		}

		DbgFree(inst->content);
		inst->content = tmp;
	}

	if (strlen(title)) {
		char *tmp;

		tmp = strdup(title);
		if (!tmp) {
			ErrPrint("strdup: %d\n", errno);
			goto out;
		}

		DbgFree(inst->title);
		inst->title = tmp;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		goto out;
	}

	switch (ret) {
	case 0: /*!< normally created */
		inst->state = INST_ACTIVATED;
		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		case INST_ACTIVATED:
			inst->is_pinned_up = is_pinned_up;
			widget_type = package_widget_type(inst->info);
			gbar_type = package_gbar_type(inst->info);

			/*!
			 * \note
			 * Optimization point.
			 *   In case of the BUFFER type,
			 *   the slave will request the buffer to render its contents.
			 *   so the buffer will be automatcially recreated when it gots the
			 *   buffer request packet.
			 *   so load a buffer from here is not neccessary.
			 *   I should to revise it and concrete the concept.
			 *   Just leave it only for now.
			 */

			if (widget_type == WIDGET_TYPE_SCRIPT && inst->widget.canvas.script) {
				script_handler_load(inst->widget.canvas.script, 0);
			} else if (widget_type == WIDGET_TYPE_BUFFER && inst->widget.canvas.buffer) {
				buffer_handler_load(inst->widget.canvas.buffer);
			}

			if (gbar_type == GBAR_TYPE_SCRIPT && inst->gbar.canvas.script && inst->gbar.is_opened_for_reactivate) {
				double x, y;
				/*!
				 * \note
				 * We should to send a request to open a GBAR to slave.
				 * if we didn't send it, the slave will not recognize the state of a GBAR.
				 * We have to keep the view of GBAR seamless even if the widget is reactivated.
				 * To do that, send open request from here.
				 */
				ret = instance_slave_open_gbar(inst, NULL);
				instance_slave_get_gbar_pos(inst, &x, &y);

				/*!
				 * \note
				 * In this case, master already loads the GBAR script.
				 * So just send the gbar,show event to the slave again.
				 */
				ret = instance_signal_emit(inst, "gbar,show", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
			} else if (gbar_type == GBAR_TYPE_BUFFER && inst->gbar.canvas.buffer && inst->gbar.is_opened_for_reactivate) {
				double x, y;

				buffer_handler_load(inst->gbar.canvas.buffer);
				instance_slave_get_gbar_pos(inst, &x, &y);

				/*!
				 * \note
				 * We should to send a request to open a GBAR to slave.
				 * if we didn't send it, the slave will not recognize the state of a GBAR.
				 * We have to keep the view of GBAR seamless even if the widget is reactivated.
				 * To do that, send open request from here.
				 */
				ret = instance_slave_open_gbar(inst, NULL);

				/*!
				 * \note
				 * In this case, just send the gbar,show event for keeping the compatibility
				 */
				ret = instance_signal_emit(inst, "gbar,show", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
			}

			/*!
			 * \note
			 * After create an instance again,
			 * Send resize request to the widget.
			 * instance_resize(inst, inst->widget.width, inst->widget.height);
			 *
			 * renew request will resize the widget while creating it again
			 */

			/*!
			 * \note
			 * This function will check the visiblity of a widget and
			 * make decision whether it thaw the update timer or not.
			 */
			instance_recover_visible_state(inst);
		default:
			break;
		}
		break;
	default:
		instance_broadcast_deleted_event(inst, ret);
		instance_state_reset(inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
		break;
	}

out:
	inst->changing_state--;
	if (inst->changing_state < 0) {
		ErrPrint("Changing state is not correct: %d\n", inst->changing_state);
	}
	instance_unref(inst);
}

static void activate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	int ret;
	int w;
	int h;
	double priority;
	char *content;
	char *title;
	int is_pinned_up;

	if (!packet) {
		/*!
		 * \note
		 * instance_reload will care this
		 * it will be called from the slave_activate callback
		 */
		goto out;
	}

	if (packet_get(packet, "iiidssi", &ret, &w, &h, &priority, &content, &title, &is_pinned_up) != 7) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	DbgPrint("[%s] returns %d (state: 0x%X)\n", inst->id, ret, inst->state);

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		goto out;
	}

	switch (ret) {
	case 1: /*!< need to create */
		if (util_free_space(WIDGET_CONF_IMAGE_PATH) > WIDGET_CONF_MINIMUM_SPACE) {
			struct inst_info *new_inst;
			new_inst = instance_create(inst->client, util_timestamp(), package_name(inst->info),
					inst->content, inst->cluster, inst->category,
					inst->widget.period, 0, 0);
			if (!new_inst) {
				ErrPrint("Failed to create a new instance\n");
			}
		} else {
			ErrPrint("Not enough space\n");
		}
	case 0: /*!< normally created */
		/*!
		 * \note
		 * Anyway this instance is loaded to the slave,
		 * just increase the loaded instance counter
		 * And then reset jobs.
		 */
		instance_set_widget_size(inst, w, h);
		instance_set_widget_info(inst, priority, content, title);

		inst->state = INST_ACTIVATED;

		switch (inst->requested_state) {
		case INST_DESTROYED:
			/**
			 * In this case, we should destroy the instance.
			 */
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		case INST_ACTIVATED:
		default:
			/**
			 * @note
			 * WIDGET should be created at the create time
			 */
			inst->is_pinned_up = is_pinned_up;
			if (package_widget_type(inst->info) == WIDGET_TYPE_SCRIPT) {
				if (inst->widget.width == 0 && inst->widget.height == 0) {
					widget_service_get_size(WIDGET_SIZE_TYPE_1x1, &inst->widget.width, &inst->widget.height);
				}

				inst->widget.canvas.script = script_handler_create(inst,
						package_widget_path(inst->info),
						package_widget_group(inst->info),
						inst->widget.width, inst->widget.height);

				if (!inst->widget.canvas.script) {
					ErrPrint("Failed to create WIDGET\n");
				} else {
					script_handler_load(inst->widget.canvas.script, 0);
				}
			} else if (package_widget_type(inst->info) == WIDGET_TYPE_BUFFER) {
				instance_create_widget_buffer(inst, WIDGET_CONF_DEFAULT_PIXELS);
			}

			if (package_gbar_type(inst->info) == GBAR_TYPE_SCRIPT) {
				if (inst->gbar.width == 0 && inst->gbar.height == 0) {
					instance_set_gbar_size(inst, package_gbar_width(inst->info), package_gbar_height(inst->info));
				}

				inst->gbar.canvas.script = script_handler_create(inst,
						package_gbar_path(inst->info),
						package_gbar_group(inst->info),
						inst->gbar.width, inst->gbar.height);

				if (!inst->gbar.canvas.script) {
					ErrPrint("Failed to create GBAR\n");
				}
			} else if (package_gbar_type(inst->info) == GBAR_TYPE_BUFFER) {
				instance_create_gbar_buffer(inst, WIDGET_CONF_DEFAULT_PIXELS);
			}

			instance_broadcast_created_event(inst);

			instance_thaw_updator(inst);
			break;
		}
		break;
	default:
		instance_unicast_deleted_event(inst, NULL, ret);
		instance_state_reset(inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
		break;
	}

out:
	inst->changing_state--;
	if (inst->changing_state < 0) {
		ErrPrint("Changing state is not correct: %d\n", inst->changing_state);
	}
	instance_unref(inst);
}

HAPI int instance_create_gbar_buffer(struct inst_info *inst, int pixels)
{
	if (inst->gbar.width == 0 && inst->gbar.height == 0) {
		instance_set_gbar_size(inst, package_gbar_width(inst->info), package_gbar_height(inst->info));
	}

	if (!inst->gbar.canvas.buffer) {
		inst->gbar.canvas.buffer = buffer_handler_create(inst, s_info.env_buf_type, inst->gbar.width, inst->gbar.height, pixels);
		if (!inst->gbar.canvas.buffer) {
			ErrPrint("Failed to create GBAR Buffer\n");
		}
	}

	return !!inst->gbar.canvas.buffer;
}

HAPI int instance_create_gbar_extra_buffer(struct inst_info *inst, int pixels, int idx)
{
	if (!inst->gbar.extra_buffer) {
		return 0;
	}

	if (inst->gbar.width == 0 && inst->gbar.height == 0) {
		instance_set_gbar_size(inst, package_gbar_width(inst->info), package_gbar_height(inst->info));
	}

	if (!inst->gbar.extra_buffer[idx]) {
		inst->gbar.extra_buffer[idx] = buffer_handler_create(inst, s_info.env_buf_type, inst->gbar.width, inst->gbar.height, pixels);
		if (!inst->gbar.extra_buffer[idx]) {
			ErrPrint("Failed to create GBAR Extra Buffer\n");
		}
	}

	return !!inst->gbar.extra_buffer[idx];
}

HAPI int instance_create_widget_buffer(struct inst_info *inst, int pixels)
{
	if (inst->widget.width == 0 && inst->widget.height == 0) {
		widget_service_get_size(WIDGET_SIZE_TYPE_1x1, &inst->widget.width, &inst->widget.height);
	}

	if (!inst->widget.canvas.buffer) {
		/*!
		 * \note
		 * Slave doesn't call the acquire_buffer.
		 * In this case, create the buffer from here.
		 */
		inst->widget.canvas.buffer = buffer_handler_create(inst, s_info.env_buf_type, inst->widget.width, inst->widget.height, pixels);
		if (!inst->widget.canvas.buffer) {
			ErrPrint("Failed to create WIDGET\n");
		}
	}

	return !!inst->widget.canvas.buffer;
}

HAPI int instance_create_widget_extra_buffer(struct inst_info *inst, int pixels, int idx)
{
	if (!inst->widget.extra_buffer) {
		return 0;
	}

	if (inst->widget.width == 0 && inst->widget.height == 0) {
		widget_service_get_size(WIDGET_SIZE_TYPE_1x1, &inst->widget.width, &inst->widget.height);
	}

	if (!inst->widget.extra_buffer[idx]) {
		inst->widget.extra_buffer[idx] = buffer_handler_create(inst, s_info.env_buf_type, inst->widget.width, inst->widget.height, pixels);
		if (!inst->widget.extra_buffer[idx]) {
			ErrPrint("Failed to create widget Extra buffer\n");
		}
	}

	return !!inst->widget.extra_buffer[idx];
}

HAPI int instance_destroy(struct inst_info *inst, widget_destroy_type_e type)
{
	struct packet *packet;
	unsigned int cmd = CMD_DELETE;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_REQUEST_TO_REACTIVATE:
		inst->requested_state = INST_DESTROYED;
		return WIDGET_ERROR_NONE;
	case INST_INIT:
		inst->state = INST_DESTROYED;
		inst->requested_state = INST_DESTROYED;
		(void)instance_unref(inst);
		return WIDGET_ERROR_NONE;
	case INST_DESTROYED:
		inst->requested_state = INST_DESTROYED;
		return WIDGET_ERROR_NONE;
	default:
		if (type == WIDGET_DESTROY_TYPE_UNINSTALL || type == WIDGET_DESTROY_TYPE_UPGRADE) {
			struct pkg_info *pkg;

			/**
			 * @note
			 * In this case, we cannot re-activate the slave.
			 * Because it is already uninstalled so there is no widget application anymore.
			 * So just clear the instances from the homescreen
			 */
			pkg = instance_package(inst);
			if (pkg) {
				struct slave_node *slave;

				slave = package_slave(pkg);
				if (slave) {
					/**
					 * @note
					 * If a slave is not activated, (already deactivated)
					 * We don't need to try to destroy an instance.
					 * Just delete an instance from here.
					 */
					if (!slave_is_activated(slave)) {
						/**
						 * @note
						 * Notify deleted instance information to the viewer.
						 */
						if (inst->unicast_delete_event) {
							instance_unicast_deleted_event(inst, NULL, WIDGET_ERROR_NONE);
						} else {
							instance_broadcast_deleted_event(inst, WIDGET_ERROR_NONE);
						}

						DbgPrint("Slave is deactivated, delete an instance\n");
						inst->state = INST_DESTROYED;
						inst->requested_state = INST_DESTROYED;
						(void)instance_unref(inst);
						return WIDGET_ERROR_NONE;
					}
				}
			}
		}

		break;
	}

	packet = packet_create((const char *)&cmd, "ssi", package_name(inst->info), inst->id, type);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	inst->destroy_type = type;
	inst->requested_state = INST_DESTROYED;
	inst->state = INST_REQUEST_TO_DESTROY;
	inst->changing_state++;
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, deactivate_cb, instance_ref(inst), 0);
}

HAPI int instance_reload(struct inst_info *inst, widget_destroy_type_e type)
{
	struct packet *packet;
	unsigned int cmd = CMD_DELETE;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	DbgPrint("Reload instance (%s)\n", instance_id(inst));

	switch (inst->state) {
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_REACTIVATE:
		return WIDGET_ERROR_NONE;
	case INST_INIT:
		ret = instance_activate(inst);
		if (ret < 0) {
			ErrPrint("Failed to activate instance: %d (%s)\n", ret, instance_id(inst));
		}
		return WIDGET_ERROR_NONE;
	case INST_DESTROYED:
	case INST_REQUEST_TO_DESTROY:
		DbgPrint("Instance is destroying now\n");
		return WIDGET_ERROR_NONE;
	default:
		if (type == WIDGET_DESTROY_TYPE_UNINSTALL || type == WIDGET_DESTROY_TYPE_UPGRADE) {
			struct pkg_info *pkg;

			pkg = instance_package(inst);
			if (pkg) {
				struct slave_node *slave;

				slave = package_slave(pkg);
				if (slave) {
					/**
					 * @note
					 * If a slave is not activated, (already deactivated)
					 * We don't need to try to destroy an instance.
					 * Just re-activate an instance from here.
					 */
					if (!slave_is_activated(slave)) {
						inst->state = INST_INIT;
						DbgPrint("Slave is deactivated, just activate an instasnce\n");
						ret = instance_activate(inst);
						if (ret < 0) {
							ErrPrint("Failed to activate instance: %d (%s)\n", ret, instance_id(inst));
						}
						return WIDGET_ERROR_NONE;
					}
				}
			}
		}
		break;
	}

	packet = packet_create((const char *)&cmd, "ssi", package_name(inst->info), inst->id, type);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	inst->destroy_type = type;
	inst->requested_state = INST_ACTIVATED;
	inst->state = INST_REQUEST_TO_DESTROY;
	inst->changing_state++;
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, deactivate_cb, instance_ref(inst), 0);
}

/* Client Deactivated Callback */
static int gbar_buffer_close_cb(struct client_node *client, void *inst)
{
	int ret;

	ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_NORMAL);
	if (ret < 0) {
		DbgPrint("Forcely close the GBAR ret: %d\n", ret);
	}

	instance_unref(inst);

	return -1; /* Delete this callback */
}

/* Client Deactivated Callback */
static int gbar_script_close_cb(struct client_node *client, void *inst)
{
	int ret;

	ret = script_handler_unload(instance_gbar_script(inst), 1);
	if (ret < 0) {
		DbgPrint("Unload script: %d\n", ret);
	}

	ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_NORMAL);
	if (ret < 0) {
		DbgPrint("Forcely close the GBAR ret: %d\n", ret);
	}

	instance_unref(inst);

	return -1; /* Delete this callback */
}

static inline void release_resource_for_closing_gbar(struct pkg_info *info, struct inst_info *inst, struct client_node *client)
{
	if (!client) {
		client = inst->gbar.owner;
		if (!client) {
			return;
		}
	}

	/*!
	 * \note
	 * Clean up the resources
	 */
	if (package_gbar_type(info) == GBAR_TYPE_BUFFER) {
		if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, gbar_buffer_close_cb, inst) == 0) {
			/*!
			 * \note
			 * Only if this function succeed to remove the gbar_buffer_close_cb,
			 * Decrease the reference count of this instance
			 */
			instance_unref(inst);
		}
	} else if (package_gbar_type(info) == GBAR_TYPE_SCRIPT) {
		if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, gbar_script_close_cb, inst) == 0) {
			/*!
			 * \note
			 * Only if this function succeed to remove the script_close_cb,
			 * Decrease the reference count of this instance
			 */
			instance_unref(inst);
		}
	} else {
		ErrPrint("Unknown GBAR type\n");
	}

}

HAPI int instance_state_reset(struct inst_info *inst)
{
	enum widget_widget_type widget_type;
	enum widget_gbar_type gbar_type;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (inst->state == INST_DESTROYED) {
		goto out;
	}

	widget_type = package_widget_type(inst->info);
	gbar_type = package_gbar_type(inst->info);

	if (widget_type == WIDGET_TYPE_SCRIPT && inst->widget.canvas.script) {
		script_handler_unload(inst->widget.canvas.script, 0);
	} else if (widget_type == WIDGET_TYPE_BUFFER && inst->widget.canvas.buffer) {
		buffer_handler_unload(inst->widget.canvas.buffer);
	}

	if (gbar_type == GBAR_TYPE_SCRIPT && inst->gbar.canvas.script) {
		inst->gbar.is_opened_for_reactivate = script_handler_is_loaded(inst->gbar.canvas.script);
		release_resource_for_closing_gbar(instance_package(inst), inst, NULL);
		script_handler_unload(inst->gbar.canvas.script, 1);
	} else if (gbar_type == GBAR_TYPE_BUFFER && inst->gbar.canvas.buffer) {
		inst->gbar.is_opened_for_reactivate = buffer_handler_is_loaded(inst->gbar.canvas.buffer);
		release_resource_for_closing_gbar(instance_package(inst), inst, NULL);
		buffer_handler_unload(inst->gbar.canvas.buffer);
	}

out:
	inst->state = INST_INIT;
	inst->requested_state = INST_INIT;
	return WIDGET_ERROR_NONE;
}

HAPI int instance_reactivate(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_RENEW;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_DESTROY:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_REACTIVATE:
		inst->requested_state = INST_ACTIVATED;
		return WIDGET_ERROR_NONE;
	case INST_DESTROYED:
	case INST_ACTIVATED:
		return WIDGET_ERROR_NONE;
	case INST_INIT:
	default:
		break;
	}

	packet = packet_create((const char *)&cmd, "sssiidssiisiisi",
			package_name(inst->info),
			inst->id,
			inst->content,
			package_timeout(inst->info),
			!!package_widget_path(inst->info),
			inst->widget.period,
			inst->cluster,
			inst->category,
			inst->widget.width, inst->widget.height,
			package_abi(inst->info),
			inst->scroll_locked,
			inst->active_update,
			client_direct_addr(inst->client),
			inst->orientation);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	ret = slave_activate(package_slave(inst->info));
	if (ret < 0 && ret != WIDGET_ERROR_ALREADY_STARTED) {
		/*!
		 * \note
		 * If the master failed to launch the slave,
		 * Do not send any requests to the slave.
		 */
		ErrPrint("Failed to launch the slave\n");
		packet_destroy(packet);
		return ret;
	}

	inst->requested_state = INST_ACTIVATED;
	inst->state = INST_REQUEST_TO_REACTIVATE;
	inst->changing_state++;

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, reactivate_cb, instance_ref(inst), 1);
}

HAPI int instance_activate(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_NEW;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		inst->requested_state = INST_ACTIVATED;
		return WIDGET_ERROR_NONE;
	case INST_ACTIVATED:
	case INST_DESTROYED:
		return WIDGET_ERROR_NONE;
	case INST_INIT:
	default:
		break;
	}

	packet = packet_create((const char *)&cmd, "sssiidssisiisi",
			package_name(inst->info),
			inst->id,
			inst->content,
			package_timeout(inst->info),
			!!package_widget_path(inst->info),
			inst->widget.period,
			inst->cluster,
			inst->category,
			!!inst->client,
			package_abi(inst->info),
			inst->widget.width,
			inst->widget.height,
			client_direct_addr(inst->client),
			inst->orientation);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	ret = slave_activate(package_slave(inst->info));
	if (ret < 0 && ret != WIDGET_ERROR_ALREADY_STARTED) {
		/*!
		 * \note
		 * If the master failed to launch the slave,
		 * Do not send any requests to the slave.
		 */
		ErrPrint("Failed to launch the slave\n");
		packet_destroy(packet);
		return ret;
	}

	inst->state = INST_REQUEST_TO_ACTIVATE;
	inst->requested_state = INST_ACTIVATED;
	inst->changing_state++;

	/*!
	 * \note
	 * Try to activate a slave if it is not activated
	 */
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, activate_cb, instance_ref(inst), 1);
}

HAPI int instance_widget_update_begin(struct inst_info *inst, double priority, const char *content, const char *title)
{
	struct packet *packet;
	const char *fbfile;
	unsigned int cmd = CMD_WIDGET_UPDATE_BEGIN;

	if (!inst->active_update) {
		ErrPrint("Invalid request [%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (package_widget_type(inst->info)) {
	case WIDGET_TYPE_BUFFER:
		if (!inst->widget.canvas.buffer) {
			ErrPrint("Buffer is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		fbfile = buffer_handler_id(inst->widget.canvas.buffer);
		break;
	case WIDGET_TYPE_SCRIPT:
		if (!inst->widget.canvas.script) {
			ErrPrint("Script is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		fbfile = script_handler_buffer_id(inst->widget.canvas.script);
		break;
	default:
		ErrPrint("Invalid request[%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssdsss", package_name(inst->info), inst->id, priority, content, title, fbfile);
	if (!packet) {
		ErrPrint("Unable to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_widget_update_end(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_WIDGET_UPDATE_END;

	if (!inst->active_update) {
		ErrPrint("Invalid request [%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (package_widget_type(inst->info)) {
	case WIDGET_TYPE_BUFFER:
		if (!inst->widget.canvas.buffer) {
			ErrPrint("Buffer is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		break;
	case WIDGET_TYPE_SCRIPT:
		if (!inst->widget.canvas.script) {
			ErrPrint("Script is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		break;
	default:
		ErrPrint("Invalid request[%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Unable to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_gbar_update_begin(struct inst_info *inst)
{
	struct packet *packet;
	const char *fbfile;
	unsigned int cmd = CMD_GBAR_UPDATE_BEGIN;

	if (!inst->active_update) {
		ErrPrint("Invalid request [%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (package_gbar_type(inst->info)) {
	case GBAR_TYPE_BUFFER:
		if (!inst->gbar.canvas.buffer) {
			ErrPrint("Buffer is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		fbfile = buffer_handler_id(inst->gbar.canvas.buffer);
		break;
	case GBAR_TYPE_SCRIPT:
		if (!inst->gbar.canvas.script) {
			ErrPrint("Script is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		fbfile = script_handler_buffer_id(inst->gbar.canvas.script);
		break;
	default:
		ErrPrint("Invalid request[%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "sss", package_name(inst->info), inst->id, fbfile);
	if (!packet) {
		ErrPrint("Unable to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_gbar_update_end(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_GBAR_UPDATE_END;

	if (!inst->active_update) {
		ErrPrint("Invalid request [%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (package_gbar_type(inst->info)) {
	case GBAR_TYPE_BUFFER:
		if (!inst->gbar.canvas.buffer) {
			ErrPrint("Buffer is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		break;
	case GBAR_TYPE_SCRIPT:
		if (!inst->gbar.canvas.script) {
			ErrPrint("Script is null [%s]\n", inst->id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
		break;
	default:
		ErrPrint("Invalid request[%s]\n", inst->id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Unable to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI void instance_extra_info_updated_by_instance(struct inst_info *inst)
{
	struct packet *packet;
	unsigned int cmd = CMD_EXTRA_INFO;

	packet = packet_create_noack((const char *)&cmd, "ssssssd", package_name(inst->info), inst->id,
			inst->content, inst->title,
			inst->icon, inst->name,
			inst->widget.priority);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)client_send_event(inst, packet, NULL);
}

HAPI void instance_extra_updated_by_instance(struct inst_info *inst, int is_gbar, int idx, int x, int y, int w, int h)
{
	struct packet *packet;
	enum widget_widget_type widget_type;
	unsigned int cmd = CMD_EXTRA_UPDATED;

	if (idx < 0 || idx > WIDGET_CONF_EXTRA_BUFFER_COUNT) {
		ErrPrint("Invalid index\n");
		return;
	}

	if (is_gbar == 0) {
		if (!inst->widget.extra_buffer || inst->widget.extra_buffer[idx] == 0u) {
			ErrPrint("Invalid extra buffer\n");
			return;
		}
	} else {
		if (!inst->gbar.extra_buffer || inst->gbar.extra_buffer[idx] == 0u) {
			ErrPrint("Invalid extra buffer\n");
			return;
		}
	}

	if (inst->client && inst->visible != WIDGET_SHOW) {
		if (inst->visible == WIDGET_HIDE) {
			DbgPrint("Ignore update event %s(HIDE)\n", inst->id);
			return;
		}
	}

	widget_type = package_widget_type(inst->info);
	if (widget_type != WIDGET_TYPE_BUFFER) {
		ErrPrint("Unsupported type\n");
		return;
	}

	packet = packet_create_noack((const char *)&cmd, "ssiiiiii", package_name(inst->info), inst->id, is_gbar, idx, x, y, w, h);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)client_send_event(inst, packet, NULL);
}

HAPI void instance_widget_updated_by_instance(struct inst_info *inst, const char *safe_file, int x, int y, int w, int h)
{
	struct packet *packet;
	const char *id = NULL;
	enum widget_widget_type widget_type;
	unsigned int cmd = CMD_WIDGET_UPDATED;

	if (inst->client && inst->visible != WIDGET_SHOW) {
		if (inst->visible == WIDGET_HIDE) {
			DbgPrint("Ignore update event %s(HIDE)\n", inst->id);
			return;
		}
	}

	widget_type = package_widget_type(inst->info);
	if (widget_type == WIDGET_TYPE_SCRIPT) {
		id = script_handler_buffer_id(inst->widget.canvas.script);
	} else if (widget_type == WIDGET_TYPE_BUFFER) {
		id = buffer_handler_id(inst->widget.canvas.buffer);
	}

	packet = packet_create_noack((const char *)&cmd, "ssssiiii", package_name(inst->info), inst->id, id, safe_file, x, y, w, h);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)client_send_event(inst, packet, NULL);
}

HAPI int instance_hold_scroll(struct inst_info *inst, int hold)
{
	struct packet *packet;
	unsigned int cmd = CMD_SCROLL;

	DbgPrint("HOLD: (%s) %d\n", inst->id, hold);
	if (inst->scroll_locked == hold) {
		return WIDGET_ERROR_ALREADY_EXIST;
	}

	packet = packet_create_noack((const char *)&cmd, "ssi", package_name(inst->info), inst->id, hold);
	if (!packet) {
		ErrPrint("Failed to build a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	inst->scroll_locked = hold;
	return client_send_event(inst, packet, NULL);
}

HAPI void instance_gbar_updated_by_instance(struct inst_info *inst, const char *descfile, int x, int y, int w, int h)
{
	struct packet *packet;
	unsigned int cmd = CMD_GBAR_UPDATED;
	const char *id;

	if (inst->client && inst->visible != WIDGET_SHOW) {
		DbgPrint("widget is hidden. ignore update event\n");
		return;
	}

	if (!inst->gbar.need_to_send_close_event) {
		DbgPrint("GBAR is not created yet. Ignore update event - %s\n", descfile);

		if (inst->gbar.pended_update_desc) {
			DbgFree(inst->gbar.pended_update_desc);
			inst->gbar.pended_update_desc = NULL;
		}

		if (descfile) {
			inst->gbar.pended_update_desc = strdup(descfile);
			if (!inst->gbar.pended_update_desc) {
				ErrPrint("strdup: %d\n", errno);
			}
		}

		inst->gbar.pended_update_cnt++;
		return;
	}

	if (!descfile) {
		descfile = inst->id;
	}

	switch (package_gbar_type(inst->info)) {
	case GBAR_TYPE_SCRIPT:
		id = script_handler_buffer_id(inst->gbar.canvas.script);
		break;
	case GBAR_TYPE_BUFFER:
		id = buffer_handler_id(inst->gbar.canvas.buffer);
		break;
	case GBAR_TYPE_TEXT:
	default:
		id = "";
		break;
	}

	packet = packet_create_noack((const char *)&cmd, "ssssiiii", package_name(inst->info), inst->id, id, descfile, x, y, w, h);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)client_send_event(inst, packet, NULL);
}

HAPI void instance_gbar_updated(const char *pkgname, const char *id, const char *descfile, int x, int y, int w, int h)
{
	struct inst_info *inst;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		return;
	}

	instance_gbar_updated_by_instance(inst, descfile, x, y, w, h);
}

HAPI int instance_set_update_mode(struct inst_info *inst, int active_update)
{
	struct packet *packet;
	struct update_mode_cbdata *cbdata;
	unsigned int cmd = CMD_UPDATE_MODE;

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	if (inst->active_update == active_update) {
		DbgPrint("Active update is not changed: %d\n", inst->active_update);
		return WIDGET_ERROR_ALREADY_EXIST;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->inst = instance_ref(inst);
	cbdata->active_update = active_update;

	/* NOTE: param is resued from here */
	packet = packet_create((const char *)&cmd, "ssi", package_name(inst->info), inst->id, active_update);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, update_mode_cb, cbdata, 0);
}

HAPI int instance_active_update(struct inst_info *inst)
{
	return inst->active_update;
}

HAPI void instance_set_widget_info(struct inst_info *inst, double priority, const char *content, const char *title)
{
	char *_content = NULL;
	char *_title = NULL;

	if (content && strlen(content)) {
		_content = strdup(content);
		if (!_content) {
			ErrPrint("strdup: %d\n", errno);
		}
	}

	if (title && strlen(title)) {
		_title = strdup(title);
		if (!_title) {
			ErrPrint("strdup: %d\n", errno);
		}
	}

	if (_content) {
		DbgFree(inst->content);
		inst->content= _content;
	}

	if (_title) {
		DbgFree(inst->title);
		inst->title = _title;
	}

	if (priority >= 0.0f && priority <= 1.0f) {
		inst->widget.priority = priority;
	}
}

HAPI void instance_set_alt_info(struct inst_info *inst, const char *icon, const char *name)
{
	char *_icon = NULL;
	char *_name = NULL;

	if (icon && strlen(icon)) {
		_icon = strdup(icon);
		if (!_icon) {
			ErrPrint("strdup: %d\n", errno);
		}
	}

	if (name && strlen(name)) {
		_name = strdup(name);
		if (!_name) {
			ErrPrint("strdup: %d\n", errno);
		}
	}

	if (_icon) {
		DbgFree(inst->icon);
		inst->icon = _icon;
	}

	if (_name) {
		DbgFree(inst->name);
		inst->name = _name;
	}
}

HAPI void instance_set_widget_size(struct inst_info *inst, int w, int h)
{
	if (inst->widget.width != w || inst->widget.height != h) {
		instance_send_resized_event(inst, IS_WIDGET, w, h, WIDGET_ERROR_NONE);
	}

	inst->widget.width = w;
	inst->widget.height = h;
}

HAPI void instance_set_gbar_size(struct inst_info *inst, int w, int h)
{
	if (inst->gbar.width != w || inst->gbar.height != h) {
		instance_send_resized_event(inst, IS_GBAR, w, h, WIDGET_ERROR_NONE);
	}

	inst->gbar.width = w;
	inst->gbar.height = h;
}

static void pinup_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct set_pinup_cbdata *cbdata = data;
	const char *content;
	struct packet *result;
	unsigned int cmd = CMD_RESULT_PINUP;
	int ret;

	if (!packet) {
		/*!
		 * \todo
		 * Send pinup failed event to client.
		 */
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (packet_get(packet, "is", &ret, &content) != 2) {
		/*!
		 * \todo
		 * Send pinup failed event to client
		 */
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (ret == 0) {
		char *new_content;

		new_content = strdup(content);
		if (!new_content) {
			ErrPrint("strdup: %d\n", errno);
			/*!
			 * \note
			 * send pinup failed event to client
			 */
			ret = WIDGET_ERROR_OUT_OF_MEMORY;
			goto out;
		}

		cbdata->inst->is_pinned_up = cbdata->pinup;
		DbgFree(cbdata->inst->content);

		cbdata->inst->content = new_content;
	}

out:
	/*!
	 * \node
	 * Send PINUP Result to client.
	 * Client should wait this event.
	 */
	result = packet_create_noack((const char *)&cmd, "iisss", ret, cbdata->inst->is_pinned_up,
			package_name(cbdata->inst->info), cbdata->inst->id, cbdata->inst->content);
	if (result) {
		(void)client_send_event(cbdata->inst, result, NULL);
	} else {
		ErrPrint("Failed to build a packet for %s\n", package_name(cbdata->inst->info));
	}

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_set_pinup(struct inst_info *inst, int pinup)
{
	struct set_pinup_cbdata *cbdata;
	struct packet *packet;
	unsigned int cmd = CMD_PINUP;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	if (!package_pinup(inst->info)) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (pinup == inst->is_pinned_up) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->inst = instance_ref(inst);
	cbdata->pinup = pinup;

	packet = packet_create((const char *)&cmd, "ssi", package_name(inst->info), inst->id, pinup);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, pinup_cb, cbdata, 0);
}

HAPI int instance_freeze_updator(struct inst_info *inst)
{
	if (WIDGET_CONF_UPDATE_ON_PAUSE) {
		return WIDGET_ERROR_DISABLED;
	}

	if (!inst->update_timer) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	timer_freeze(inst);
	return WIDGET_ERROR_NONE;
}

HAPI int instance_thaw_updator(struct inst_info *inst)
{
	if (!inst->update_timer) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (client_is_all_paused() || setting_is_lcd_off()) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (inst->visible == WIDGET_HIDE_WITH_PAUSE) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	timer_thaw(inst);
	return WIDGET_ERROR_NONE;
}

HAPI enum widget_visible_state instance_visible_state(struct inst_info *inst)
{
	return inst->visible;
}

HAPI int instance_set_visible_state(struct inst_info *inst, enum widget_visible_state state)
{
	if (inst->visible == state) {
		return WIDGET_ERROR_NONE;
	}

	switch (state) {
	case WIDGET_SHOW:
	case WIDGET_HIDE:
		if (inst->visible == WIDGET_HIDE_WITH_PAUSE) {
			if (resume_widget(inst) == 0) {
				inst->visible = state;
			}

			instance_thaw_updator(inst);
		} else {
			inst->visible = state;
		}
		monitor_multicast_state_change_event(package_name(inst->info), MONITOR_EVENT_RESUMED, instance_id(inst), instance_content(inst));
		break;

	case WIDGET_HIDE_WITH_PAUSE:
		if (pause_widget(inst) == 0) {
			inst->visible = WIDGET_HIDE_WITH_PAUSE;
		}

		(void)instance_freeze_updator(inst);
		monitor_multicast_state_change_event(package_name(inst->info), MONITOR_EVENT_PAUSED, instance_id(inst), instance_content(inst));
		break;

	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	return WIDGET_ERROR_NONE;
}

/**
 * @note
 * Just touch the visible state again.
 */
HAPI int instance_watch_recover_visible_state(struct inst_info *inst)
{
	switch (inst->visible) {
	case WIDGET_SHOW:
	case WIDGET_HIDE:
		(void)resume_widget(inst);
		instance_thaw_updator(inst);
		/**
		 * @note
		 * We don't need to send the monitor event.
		 */
		break;
	case WIDGET_HIDE_WITH_PAUSE:
		(void)pause_widget(inst);
		(void)instance_freeze_updator(inst);
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	return WIDGET_ERROR_NONE;
}

static void resize_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct resize_cbdata *cbdata = data;
	int ret;

	if (!packet) {
		ErrPrint("RESIZE: Invalid packet\n");
		instance_send_resized_event(cbdata->inst, IS_WIDGET, cbdata->inst->widget.width, cbdata->inst->widget.height, WIDGET_ERROR_FAULT);
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("RESIZE: Invalid parameter\n");
		instance_send_resized_event(cbdata->inst, IS_WIDGET, cbdata->inst->widget.width, cbdata->inst->widget.height, WIDGET_ERROR_INVALID_PARAMETER);
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return;
	}

	if (ret == (int)WIDGET_ERROR_NONE) {
		/*!
		 * \note
		 * else waiting the first update with new size
		 */
		if (cbdata->inst->widget.width == cbdata->w && cbdata->inst->widget.height == cbdata->h) {
			/*!
			 * \note
			 * Right after the viewer adds a new widget,
			 * Box has no size information, then it will try to use the default size,
			 * After a widget returns created event.
			 *
			 * A widget will start to generate default size content.
			 * But the viewer doesn't know it,.
			 *
			 * So the viewer will try to change the size of a widget.
			 *
			 * At that time, the provider gots the size changed event from the widget.
			 * So it sent the size changed event to the viewer.
			 * But the viewer ignores it. if it doesn't care the size changed event.
			 * (even if it cares the size changed event, there is a timing issue)
			 *
			 * And the provider receives resize request,
			 * right before send the size changed event.
			 * but there is no changes about the size.
			 *
			 * Now the view will waits size changed event forever.
			 * To resolve this timing issue.
			 *
			 * Check the size of a widget from here.
			 * And if the size is already updated, send the ALREADY event to the viewer
			 * to get the size changed event callback correctly.
			 */
			instance_send_resized_event(cbdata->inst, IS_WIDGET, cbdata->inst->widget.width, cbdata->inst->widget.height, WIDGET_ERROR_ALREADY_EXIST);
			DbgPrint("RESIZE: widget is already resized [%s - %dx%d]\n", instance_id(cbdata->inst), cbdata->w, cbdata->h);
		} else {
			DbgPrint("RESIZE: Request is successfully sent [%s - %dx%d]\n", instance_id(cbdata->inst), cbdata->w, cbdata->h);
		}
	} else {
		DbgPrint("RESIZE: widget rejects the new size: %s - %dx%d (%d)\n", instance_id(cbdata->inst), cbdata->w, cbdata->h, ret);
		instance_send_resized_event(cbdata->inst, IS_WIDGET, cbdata->inst->widget.width, cbdata->inst->widget.height, ret);
	}

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_resize(struct inst_info *inst, int w, int h)
{
	struct resize_cbdata *cbdata;
	struct packet *packet;
	unsigned int cmd = CMD_RESIZE;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package: %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->inst = instance_ref(inst);
	cbdata->w = w;
	cbdata->h = h;

	/* NOTE: param is resued from here */
	packet = packet_create((const char *)&cmd, "ssii", package_name(inst->info), inst->id, w, h);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return WIDGET_ERROR_FAULT;
	}

	DbgPrint("RESIZE: [%s] resize[%dx%d]\n", instance_id(inst), w, h);
	ret = slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, resize_cb, cbdata, 0);
	return ret;
}

static void set_period_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct period_cbdata *cbdata = data;
	unsigned int cmd = CMD_PERIOD_CHANGED;
	struct packet *result;
	int ret;

	if (!packet) {
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (ret == 0) {
		cbdata->inst->widget.period = cbdata->period;
	} else {
		ErrPrint("Failed to set period %d\n", ret);
	}

out:
	result = packet_create_noack((const char *)&cmd, "idss", ret, cbdata->inst->widget.period, package_name(cbdata->inst->info), cbdata->inst->id);
	if (result) {
		(void)client_send_event(cbdata->inst, result, NULL);
	} else {
		ErrPrint("Failed to build a packet for %s\n", package_name(cbdata->inst->info));
	}

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
	return;
}

static Eina_Bool timer_updator_cb(void *data)
{
	struct period_cbdata *cbdata = data;
	struct inst_info *inst;
	struct packet *result;
	unsigned int cmd = CMD_PERIOD_CHANGED;
	double period;

	period = cbdata->period;
	inst = cbdata->inst;
	DbgFree(cbdata);

	inst->widget.period = period;
	if (inst->update_timer) {
		if (inst->widget.period == 0.0f) {
			ecore_timer_del(inst->update_timer);
			inst->update_timer = NULL;
		} else {
			util_timer_interval_set(inst->update_timer, inst->widget.period);
		}
	} else if (inst->widget.period > 0.0f) {
		inst->update_timer = util_timer_add(inst->widget.period, update_timer_cb, inst);
		(void)instance_freeze_updator(inst);
	}

	result = packet_create_noack((const char *)&cmd, "idss", 0, inst->widget.period, package_name(inst->info), inst->id);
	if (result) {
		(void)client_send_event(inst, result, NULL);
	} else {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
	}

	instance_unref(inst);
	return ECORE_CALLBACK_CANCEL;
}

HAPI void instance_reload_period(struct inst_info *inst, double period)
{
	inst->widget.period = period;
}

HAPI int instance_set_period(struct inst_info *inst, double period)
{
	struct packet *packet;
	struct period_cbdata *cbdata;
	unsigned int cmd = CMD_SET_PERIOD;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	if (period < 0.0f) { /* Use the default period */
		period = package_period(inst->info);
	} else if (period > 0.0f && period < WIDGET_CONF_MINIMUM_PERIOD) {
		period = WIDGET_CONF_MINIMUM_PERIOD; /* defined at conf.h */
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->period = period;
	cbdata->inst = instance_ref(inst);

	if (package_secured(inst->info) || (WIDGET_IS_INHOUSE(package_abi(inst->info)) && WIDGET_CONF_SLAVE_LIMIT_TO_TTL)) {
		/*!
		 * \note
		 * Secured widget doesn't need to send its update period to the slave.
		 * Slave has no local timer for updating widgetes
		 *
		 * So update its timer at here.
		 */
		if (!ecore_timer_add(DELAY_TIME, timer_updator_cb, cbdata)) {
			timer_updator_cb(cbdata);
		}
		return WIDGET_ERROR_NONE;
	}

	packet = packet_create((const char *)&cmd, "ssd", package_name(inst->info), inst->id, period);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, set_period_cb, cbdata, 0);
}

HAPI int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y)
{
	struct packet *packet;
	unsigned int cmd = CMD_CLICKED;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	/* NOTE: param is resued from here */
	packet = packet_create_noack((const char *)&cmd, "sssddd", package_name(inst->info), inst->id, event, timestamp, x, y);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

HAPI int instance_signal_emit(struct inst_info *inst, const char *signal, const char *part, double sx, double sy, double ex, double ey, double x, double y, int down)
{
	const char *pkgname;
	const char *id;
	struct slave_node *slave;
	struct packet *packet;
	struct pkg_info *pkg;
	unsigned int cmd = CMD_SCRIPT;

	pkg = instance_package(inst);
	pkgname = package_name(pkg);
	id = instance_id(inst);
	if (!pkgname || !id) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssssddddddi",
			pkgname, id,
			signal, part,
			sx, sy, ex, ey,
			x, y, down);
	if (!packet) {
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, pkgname, packet, 0); 
}

HAPI int instance_text_signal_emit(struct inst_info *inst, const char *signal_name, const char *source, double sx, double sy, double ex, double ey)
{
	struct packet *packet;
	unsigned int cmd = CMD_TEXT_SIGNAL;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	packet = packet_create_noack((const char *)&cmd, "ssssdddd", package_name(inst->info), inst->id, signal_name, source, sx, sy, ex, ey);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

static void change_group_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct change_group_cbdata *cbdata = data;
	struct packet *result;
	unsigned int cmd = CMD_GROUP_CHANGED;
	int ret;

	if (!packet) {
		DbgFree(cbdata->cluster);
		DbgFree(cbdata->category);
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid packet\n");
		DbgFree(cbdata->cluster);
		DbgFree(cbdata->category);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (ret == 0) {
		DbgFree(cbdata->inst->cluster);
		cbdata->inst->cluster = cbdata->cluster;

		DbgFree(cbdata->inst->category);
		cbdata->inst->category = cbdata->category;
	} else {
		DbgFree(cbdata->cluster);
		DbgFree(cbdata->category);
	}

out:
	result = packet_create_noack((const char *)&cmd, "ssiss",
			package_name(cbdata->inst->info), cbdata->inst->id, ret,
			cbdata->inst->cluster, cbdata->inst->category);
	if (!result) {
		ErrPrint("Failed to build a packet %s\n", package_name(cbdata->inst->info));
	} else {
		(void)client_send_event(cbdata->inst, result, NULL);
	}

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_change_group(struct inst_info *inst, const char *cluster, const char *category)
{
	struct packet *packet;
	struct change_group_cbdata *cbdata;
	unsigned int cmd = CMD_CHANGE_GROUP;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package [%s]\n", package_name(inst->info));
		return WIDGET_ERROR_FAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("mlloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->cluster = strdup(cluster);
	if (!cbdata->cluster) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(cbdata);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->category = strdup(category);
	if (!cbdata->category) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(cbdata->cluster);
		DbgFree(cbdata);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	cbdata->inst = instance_ref(inst);

	packet = packet_create((const char *)&cmd, "ssss", package_name(inst->info), inst->id, cluster, category);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata->category);
		DbgFree(cbdata->cluster);
		DbgFree(cbdata);
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, change_group_cb, cbdata, 0);
}

HAPI const char * const instance_auto_launch(const struct inst_info *inst)
{
	return package_auto_launch(inst->info);
}

HAPI const int const instance_priority(const struct inst_info *inst)
{
	return inst->widget.priority;
}

HAPI const struct client_node *const instance_client(const struct inst_info *inst)
{
	return inst->client;
}

HAPI const int const instance_timeout(const struct inst_info *inst)
{
	return package_timeout(inst->info);
}

HAPI const double const instance_period(const struct inst_info *inst)
{
	return inst->widget.period;
}

HAPI const int const instance_widget_width(const struct inst_info *inst)
{
	return inst->widget.width;
}

HAPI const int const instance_widget_height(const struct inst_info *inst)
{
	return inst->widget.height;
}

HAPI const int const instance_gbar_width(const struct inst_info *inst)
{
	return inst->gbar.width;
}

HAPI const int const instance_gbar_height(const struct inst_info *inst)
{
	return inst->gbar.height;
}

HAPI struct pkg_info *const instance_package(const struct inst_info *inst)
{
	return inst->info;
}

HAPI struct script_info *const instance_widget_script(const struct inst_info *inst)
{
	return (package_widget_type(inst->info) == WIDGET_TYPE_SCRIPT) ? inst->widget.canvas.script : NULL;
}

HAPI struct script_info *const instance_gbar_script(const struct inst_info *inst)
{
	return (package_gbar_type(inst->info) == GBAR_TYPE_SCRIPT) ? inst->gbar.canvas.script : NULL;
}

HAPI struct buffer_info *const instance_widget_buffer(const struct inst_info *inst)
{
	return (package_widget_type(inst->info) == WIDGET_TYPE_BUFFER) ? inst->widget.canvas.buffer : NULL;
}

HAPI struct buffer_info *const instance_gbar_buffer(const struct inst_info *inst)
{
	return (package_gbar_type(inst->info) == GBAR_TYPE_BUFFER) ? inst->gbar.canvas.buffer : NULL;
}

HAPI struct buffer_info *const instance_widget_extra_buffer(const struct inst_info *inst, int idx)
{
	return (package_widget_type(inst->info) == WIDGET_TYPE_BUFFER) ? (inst->widget.extra_buffer ? inst->widget.extra_buffer[idx] : NULL) : NULL;
}

HAPI struct buffer_info *const instance_gbar_extra_buffer(const struct inst_info *inst, int idx)
{
	return (package_gbar_type(inst->info) == GBAR_TYPE_BUFFER) ? (inst->gbar.extra_buffer ? inst->gbar.extra_buffer[idx] : NULL) : NULL;
}

HAPI const char *const instance_id(const struct inst_info *inst)
{
	return inst->id;
}

HAPI const char *const instance_content(const struct inst_info *inst)
{
	return inst->content;
}

HAPI const char *const instance_category(const struct inst_info *inst)
{
	return inst->category;
}

HAPI const char *const instance_cluster(const struct inst_info *inst)
{
	return inst->cluster;
}

HAPI const char * const instance_title(const struct inst_info *inst)
{
	return inst->title;
}

HAPI const double const instance_timestamp(const struct inst_info *inst)
{
	return inst->timestamp;
}

HAPI const enum instance_state const instance_state(const struct inst_info *inst)
{
	return inst->state;
}

HAPI int instance_destroyed(struct inst_info *inst, int reason)
{
	switch (inst->state) {
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		if (inst->unicast_delete_event) {
			/*!
			 * \note
			 * No other clients know the existence of this instance,
			 * only who added this knows it.
			 * So send deleted event to only it.
			 */
			DbgPrint("Send deleted event - unicast - %p\n", inst->client);
			instance_unicast_deleted_event(inst, NULL, reason);
		} else {
			instance_broadcast_deleted_event(inst, reason);
		}
		instance_state_reset(inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
		break;
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_ACTIVATED:
		DbgPrint("Send deleted event - multicast\n");
		instance_broadcast_deleted_event(inst, reason);
		instance_state_reset(inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
	case INST_DESTROYED:
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	return WIDGET_ERROR_NONE;
}

/*!
 * Invoked when a slave is activated
 */
HAPI int instance_recover_state(struct inst_info *inst)
{
	int ret = 0;

	if (inst->changing_state > 0) {
		DbgPrint("Doesn't need to recover the state\n");
		return WIDGET_ERROR_NONE;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Package is faulted(%s), Delete it\n", inst->id);
		inst->requested_state = INST_DESTROYED;
	}

	switch (inst->state) {
	case INST_ACTIVATED:
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			DbgPrint("Req. to RE-ACTIVATED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_reactivate(inst);
			ret = 1;
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		default:
			break;
		}
		break;
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		switch (inst->requested_state) {
		case INST_ACTIVATED:
		case INST_INIT:
			DbgPrint("Req. to ACTIVATED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			if (instance_activate(inst) < 0) {
				DbgPrint("Failed to reactivate the instance\n");
				instance_broadcast_deleted_event(inst, WIDGET_ERROR_FAULT);
				instance_state_reset(inst);
				instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			} else {
				ret = 1;
			}
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		default:
			break;
		}
		break;
	case INST_DESTROYED:
	default:
		break;
	}

	return ret;
}

/*!
 * Invoked when a slave is deactivated
 */
HAPI int instance_need_slave(struct inst_info *inst)
{
	int ret = 0;

	if (inst->client && client_is_faulted(inst->client)) {
		/*!
		 * \note
		 * In this case, the client is faulted(disconnected)
		 * when the client is deactivated, its widgetes should be removed too.
		 * So if the current inst is created by the faulted client,
		 * remove it and don't try to recover its states
		 */

		DbgPrint("CLIENT FAULT: Req. to DESTROYED (%s)\n", package_name(inst->info));
		switch (inst->state) {
		case INST_INIT:
		case INST_ACTIVATED:
		case INST_REQUEST_TO_REACTIVATE:
		case INST_REQUEST_TO_DESTROY:
		case INST_REQUEST_TO_ACTIVATE:
			instance_state_reset(inst);
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		case INST_DESTROYED:
			break;
		}

		return WIDGET_ERROR_NONE;
	}

	switch (inst->state) {
	case INST_ACTIVATED:
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		switch (inst->requested_state) {
		case INST_INIT:
		case INST_ACTIVATED:
			DbgPrint("Req. to ACTIVATED (%s)\n", package_name(inst->info));
			ret = 1;
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		default:
			break;
		}
		break;
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		switch (inst->requested_state) {
		case INST_INIT:
		case INST_ACTIVATED:
			DbgPrint("Req. to ACTIVATED (%s)\n", package_name(inst->info));
			ret = 1;
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		default:
			break;
		}
		break;
	case INST_DESTROYED:
	default:
		break;
	}

	return ret;
}

HAPI int instance_forward_packet(struct inst_info *inst, struct packet *packet)
{
	return client_send_event(inst, packet, NULL);
}

HAPI int instance_send_key_status(struct inst_info *inst, int status)
{
	struct packet *packet;
	unsigned int cmd = CMD_KEY_STATUS;

	packet = packet_create_noack((const char *)&cmd, "ssi", package_name(inst->info), inst->id, status);
	if (!packet) {
		ErrPrint("Failed to build a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_send_access_status(struct inst_info *inst, int status)
{
	struct packet *packet;
	unsigned int cmd = CMD_ACCESS_STATUS;

	packet = packet_create_noack((const char *)&cmd, "ssi", package_name(inst->info), inst->id, status);
	if (!packet) {
		ErrPrint("Failed to build a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI void instance_slave_set_gbar_pos(struct inst_info *inst, double x, double y)
{
	inst->gbar.x = x;
	inst->gbar.y = y;
}

HAPI void instance_slave_get_gbar_pos(struct inst_info *inst, double *x, double *y)
{
	if (x) {
		*x = inst->gbar.x;
	}

	if (y) {
		*y = inst->gbar.y;
	}
}

HAPI int instance_slave_open_gbar(struct inst_info *inst, struct client_node *client)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	const struct pkg_info *info;
	unsigned int cmd = CMD_GBAR_SHOW;
	int ret;

	if (!client) {
		client = inst->gbar.owner;
		if (!client) {
			ErrPrint("Client is not valid\n");
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
	} else if (inst->gbar.owner) {
		if (inst->gbar.owner != client) {
			ErrPrint("Client is already owned\n");
			return WIDGET_ERROR_ALREADY_EXIST;
		}
	}

	info = instance_package(inst);
	if (!info) {
		ErrPrint("No package info\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(info);
	if (!slave) {
		ErrPrint("No slave\n");
		return WIDGET_ERROR_FAULT;
	}

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id) {
		ErrPrint("pkgname[%s] id[%s]\n", pkgname, id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssiidd", pkgname, id, instance_gbar_width(inst), instance_gbar_height(inst), inst->gbar.x, inst->gbar.y);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	/*!
	 * \note
	 * Do not return from here even though we failed to freeze the TTL timer.
	 * Because the TTL timer is not able to be exists.
	 * So we can ignore this error.
	 */
	(void)slave_freeze_ttl(slave);

	DbgPrint("PERF_WIDGET\n");
	ret = slave_rpc_request_only(slave, pkgname, packet, 0);
	if (ret < 0) {
		ErrPrint("Unable to send request to slave\n");
		/*!
		 * \note
		 * Also we can ignore the TTL timer at here too ;)
		 */
		(void)slave_thaw_ttl(slave);
		return ret;
	}

	/*!
	 * \note
	 * If a client is disconnected, the slave has to close the GBAR
	 * So the gbar_buffer_close_cb/gbar_script_close_cb will catch the disconnection event
	 * then it will send the close request to the slave
	 */
	if (package_gbar_type(info) == GBAR_TYPE_BUFFER) {
		instance_ref(inst);
		if (client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, gbar_buffer_close_cb, inst) < 0) {
			instance_unref(inst);
		}
	} else if (package_gbar_type(info) == GBAR_TYPE_SCRIPT) {
		instance_ref(inst);
		if (client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, gbar_script_close_cb, inst) < 0) {
			instance_unref(inst);
		}
	}

	inst->gbar.owner = client;
	return ret;
}

HAPI int instance_slave_close_gbar(struct inst_info *inst, struct client_node *client, int reason)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	struct pkg_info *pkg;
	unsigned int cmd = CMD_GBAR_HIDE;
	int ret;

	if (inst->gbar.owner != client) {
		ErrPrint("Has no permission\n");
		return WIDGET_ERROR_PERMISSION_DENIED;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("No package info\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("No assigned slave\n");
		return WIDGET_ERROR_FAULT;
	}

	pkgname = package_name(pkg);
	id = instance_id(inst);

	if (!pkgname || !id) {
		ErrPrint("pkgname[%s] & id[%s] is not valid\n", pkgname, id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssi", pkgname, id, reason);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	slave_thaw_ttl(slave);

	ret = slave_rpc_request_only(slave, pkgname, packet, 0);
	release_resource_for_closing_gbar(pkg, inst, client);
	inst->gbar.owner = NULL;
	DbgPrint("PERF_WIDGET\n");
	return ret;
}

HAPI int instance_client_gbar_created(struct inst_info *inst, int status)
{
	struct packet *packet;
	const char *buf_id;
	unsigned int cmd = CMD_GBAR_CREATED;
	int ret;

	if (inst->gbar.need_to_send_close_event) {
		DbgPrint("GBAR is already created\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (package_gbar_type(inst->info)) {
	case GBAR_TYPE_SCRIPT:
		buf_id = script_handler_buffer_id(inst->gbar.canvas.script);
		break;
	case GBAR_TYPE_BUFFER:
		buf_id = buffer_handler_id(inst->gbar.canvas.buffer);
		break;
	case GBAR_TYPE_TEXT:
	default:
		buf_id = "";
		break;
	}

	inst->gbar.need_to_send_close_event = (status == 0);

	packet = packet_create_noack((const char *)&cmd, "sssiii", 
			package_name(inst->info), inst->id, buf_id,
			inst->gbar.width, inst->gbar.height, status);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	ret = client_send_event(inst, packet, NULL);

	if (inst->gbar.need_to_send_close_event && inst->gbar.pended_update_cnt) {
		DbgPrint("Apply pended desc(%d) - %s\n", inst->gbar.pended_update_cnt, inst->gbar.pended_update_desc);
		instance_gbar_updated_by_instance(inst, inst->gbar.pended_update_desc, 0, 0, inst->gbar.width, inst->gbar.height);
		inst->gbar.pended_update_cnt = 0;
		DbgFree(inst->gbar.pended_update_desc);
		inst->gbar.pended_update_desc = NULL;
	}

	return ret;
}

HAPI int instance_client_gbar_destroyed(struct inst_info *inst, int status)
{
	return send_gbar_destroyed_to_client(inst, status);
}

HAPI int instance_client_gbar_extra_buffer_created(struct inst_info *inst, int idx)
{
	struct packet *packet;
	unsigned int cmd = CMD_GBAR_CREATE_XBUF;
	int pixmap;

	pixmap = buffer_handler_pixmap(inst->gbar.extra_buffer[idx]);
	if (pixmap == 0) {
		ErrPrint("Invalid buffer\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssii", package_name(inst->info), inst->id, pixmap, idx);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_client_gbar_extra_buffer_destroyed(struct inst_info *inst, int idx)
{
	struct packet *packet;
	unsigned int cmd = CMD_GBAR_DESTROY_XBUF;
	int pixmap;

	pixmap = buffer_handler_pixmap(inst->gbar.extra_buffer[idx]);
	if (pixmap == 0) {
		ErrPrint("Invalid buffer\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssii", package_name(inst->info), inst->id, pixmap, idx);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_client_widget_extra_buffer_created(struct inst_info *inst, int idx)
{
	struct packet *packet;
	unsigned int cmd = CMD_WIDGET_CREATE_XBUF;
	int pixmap;

	pixmap = buffer_handler_pixmap(inst->widget.extra_buffer[idx]);
	if (pixmap == 0) {
		ErrPrint("Invalid buffer\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssii", package_name(inst->info), inst->id, pixmap, idx);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

HAPI int instance_client_widget_extra_buffer_destroyed(struct inst_info *inst, int idx)
{
	struct packet *packet;
	unsigned int cmd = CMD_WIDGET_DESTROY_XBUF;
	int pixmap;

	pixmap = buffer_handler_pixmap(inst->widget.extra_buffer[idx]);
	if (pixmap == 0) {
		ErrPrint("Invalid buffer\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssii", package_name(inst->info), inst->id, pixmap, idx);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	return client_send_event(inst, packet, NULL);
}

/**
 * \note
 *  this function will be called after checking the list.
 *  only if the client is not in the list, this will be called.
 */
HAPI int instance_add_client(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;
	unsigned int cmd = CMD_VIEWER_CONNECTED;

	if (inst->client == client) {
		ErrPrint("Owner cannot be the viewer\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	DbgPrint("%d is added to the list of viewer of %s(%s)\n", client_pid(client), package_name(instance_package(inst)), instance_id(inst));
	if (client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst) < 0) {
		ErrPrint("Failed to add a deactivate callback\n");
		return WIDGET_ERROR_FAULT;
	}

	packet = packet_create_noack((const char *)&cmd, "sss", package_name(inst->info), inst->id, client_direct_addr(inst->client));
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst);
		return WIDGET_ERROR_FAULT;
	}

	instance_ref(inst);
	inst->client_list = eina_list_append(inst->client_list, client);

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

HAPI int instance_del_client(struct inst_info *inst, struct client_node *client)
{
	if (inst->client == client) {
		ErrPrint("Owner is not in the viewer list\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst);
	viewer_deactivated_cb(client, inst);
	return WIDGET_ERROR_NONE;
}

HAPI int instance_has_client(struct inst_info *inst, struct client_node *client)
{
	return !!eina_list_data_find(inst->client_list, client);
}

HAPI void *instance_client_list(struct inst_info *inst)
{
	return inst->client_list;
}

HAPI int instance_init(void)
{
	if (!strcasecmp(WIDGET_CONF_PROVIDER_METHOD, "shm")) {
		s_info.env_buf_type = WIDGET_FB_TYPE_SHM;
	} else if (!strcasecmp(WIDGET_CONF_PROVIDER_METHOD, "pixmap")) {
		s_info.env_buf_type = WIDGET_FB_TYPE_PIXMAP;
	}
	/* Default method is WIDGET_FB_TYPE_FILE */

	return WIDGET_ERROR_NONE;
}

HAPI int instance_fini(void)
{
	return WIDGET_ERROR_NONE;
}

static inline struct tag_item *find_tag_item(struct inst_info *inst, const char *tag)
{
	struct tag_item *item;
	Eina_List *l;

	EINA_LIST_FOREACH(inst->data_list, l, item) {
		if (!strcmp(item->tag, tag)) {
			return item;
		}
	}

	return NULL;
}

HAPI int instance_set_data(struct inst_info *inst, const char *tag, void *data)
{
	struct tag_item *item;

	item = find_tag_item(inst, tag);
	if (!item) {
		item = malloc(sizeof(*item));
		if (!item) {
			ErrPrint("malloc: %d\n", errno);
			return WIDGET_ERROR_OUT_OF_MEMORY;
		}

		item->tag = strdup(tag);
		if (!item->tag) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(item);
			return WIDGET_ERROR_OUT_OF_MEMORY;
		}

		inst->data_list = eina_list_append(inst->data_list, item);
	}

	if (!data) {
		inst->data_list = eina_list_remove(inst->data_list, item);
		DbgFree(item->tag);
		DbgFree(item);
	} else {
		item->data = data;
	}

	return WIDGET_ERROR_NONE;
}

HAPI void *instance_del_data(struct inst_info *inst, const char *tag)
{
	struct tag_item *item;
	void *data;

	item = find_tag_item(inst, tag);
	if (!item) {
		return NULL;
	}

	inst->data_list = eina_list_remove(inst->data_list, item);
	data = item->data;
	DbgFree(item->tag);
	DbgFree(item);

	return data;
}

HAPI void *instance_get_data(struct inst_info *inst, const char *tag)
{
	struct tag_item *item;

	item = find_tag_item(inst, tag);
	if (!item) {
		return NULL;
	}

	return item->data;
}

HAPI struct client_node *instance_gbar_owner(struct inst_info *inst)
{
	return inst->gbar.owner;
}

HAPI void instance_set_orientation(struct inst_info *inst, int degree)
{
	struct packet *packet;
	unsigned int cmd = CMD_ORIENTATION;

	if (inst->orientation == degree) {
		return;
	}

	inst->orientation = degree;

	packet = packet_create_noack((const char *)&cmd, "ssi", package_name(inst->info), inst->id, degree);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return;
	}

	if (slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0) != WIDGET_ERROR_NONE) {
		/* packet will be destroyed by slave_rpc_request_only if it fails */
		ErrPrint("Failed to send a request\n");
	}

	return;
}

HAPI int instance_orientation(struct inst_info *inst)
{
	return inst->orientation;
}

HAPI void instance_watch_set_need_to_recover(struct inst_info *inst, int recover)
{
	inst->watch.need_to_recover = !!recover;
}

HAPI int instance_watch_need_to_recover(struct inst_info *inst)
{
	return inst->watch.need_to_recover;
}

HAPI int instance_watch_change_package_info(struct inst_info *inst, struct pkg_info *info)
{
	if (inst->info == info) {
		DbgPrint("Package information is not touched (%s)\n", package_name(inst->info));
		return WIDGET_ERROR_NONE;
	}

	DbgPrint("Instance[%p (%s)], info[%p (%s)]\n",
			inst->info, inst->info ? package_name(inst->info) : "unknown",
			info, info ? package_name(info) : "unknown");
	/**
	 * @todo
	 * Handling me, if the instance has package info, it means, this instance is changed its package info...
	 */
	if (inst->info != info) {
		if (inst->info) {
			ErrPrint("[%s] is already specified for [%s]\n", package_name(inst->info), instance_id(inst));
			/**
			 * @note
			 * In this case, please handling me, we have to update package info's instance list in this case.
			 */
		}

		ErrPrint("Package info is changed to [%s]\n", package_name(info));
	}

	inst->info = info;
	return WIDGET_ERROR_NONE;
}

/* End of a file */
