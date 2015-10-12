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
#include <unistd.h>
#include <errno.h>

#include <sys/smack.h>

#include <dlog.h>
#include <aul.h>
#include <Ecore.h>

#include <packet.h>
#include <com-core_packet.h>
#include <widget_errno.h>
#include <widget_service.h>
#include <widget_service_internal.h>
#include <widget_cmd_list.h>
#include <widget_conf.h>
#include <widget_abi.h>
#include <widget_script.h>
#include <widget_util.h>

#include "critical_log.h"
#include "conf.h"
#include "debug.h"
#include "server.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "script_handler.h"
#include "buffer_handler.h"
#include "util.h"
#include "fault_manager.h"
#include "group.h"
#include "xmonitor.h"
#include "widget-mgr.h"
#include "io.h"
#include "event.h"
#include "dead_monitor.h"
#include "monitor.h"

#define GBAR_OPEN_MONITOR_TAG "gbar,open,monitor"
#define GBAR_RESIZE_MONITOR_TAG "gbar,resize,monitor"
#define GBAR_CLOSE_MONITOR_TAG "gbar,close,monitor"

#define LAZY_GBAR_OPEN_TAG "lazy,gbar,open"
#define LAZY_GBAR_CLOSE_TAG "lazy,gbar,close"

#define ACCESS_TYPE_DOWN 0
#define ACCESS_TYPE_MOVE 1
#define ACCESS_TYPE_UP 2
#define ACCESS_TYPE_CUR 0
#define ACCESS_TYPE_NEXT 1
#define ACCESS_TYPE_PREV 2
#define ACCESS_TYPE_OFF 3

#define PAGE_SIZE 4096

#define aul_terminate_pid_async(a) aul_terminate_pid(a)

struct sync_ctx_item {
	int pid;
	int handle;
	char *pkgname;
	double timestamp;
	Ecore_Timer *prepare_sync_wait_timer;
};

static struct info {
	int info_fd;
	int client_fd;
	int service_fd;
	int slave_fd;

	Eina_List *hello_sync_ctx_list;
} s_info = {
	.info_fd = -1,
	.client_fd = -1,
	.service_fd = -1,
	.slave_fd = -1,
	.hello_sync_ctx_list = NULL,
};

struct access_info {
	int x;
	int y;
	int type;
};

/* Share this with provider */
enum target_type {
	TYPE_WIDGET,
	TYPE_GBAR,
	TYPE_ERROR
};

struct event_cbdata {
	int status;
	struct inst_info *inst;
};

struct deleted_item {
	struct client_node *client;
	struct inst_info *inst;
};

static int is_valid_service_requestor(pid_t pid, const char *pkgname)
{
	char pid_pkgname[pathconf("/", _PC_PATH_MAX)];
	char *caller_pkgname;
	char *widget_pkgname;
	int ret;

	if (aul_app_get_pkgname_bypid(pid, pid_pkgname, sizeof(pid_pkgname)) != AUL_R_OK) {
		ret = widget_mgr_is_valid_requestor(pid);
		if (ret == 0) {
			ErrPrint("pid[%d] is not authroized package\n", pid);
		}
		return ret;
	}

	caller_pkgname = package_get_pkgid(pid_pkgname);
	if (!caller_pkgname) {
		ErrPrint("Caller pkgname is not valid (%s, %s)\n", pid_pkgname, pkgname);
		return 0;
	}

	widget_pkgname = widget_service_get_package_id(pkgname);
	if (!widget_pkgname) {
		DbgFree(caller_pkgname);
		ErrPrint("Widget pkgname is not valid (%s, %s)\n", pid_pkgname, pkgname);
		return 0;
	}

	ret = strcmp(caller_pkgname, widget_pkgname);
	DbgFree(caller_pkgname);
	DbgFree(widget_pkgname);

	return ret == 0;
}

/**
 * Returns widget Id from provider Id
 */
static char *is_valid_slave(pid_t pid, const char *abi, const char *provider_pkgname)
{
	char pid_pkgname[pathconf("/", _PC_PATH_MAX)];
	const char *abi_pkgname;
	widget_list_h list_handle;
	char *pkgid;
	char *widget_id;
	char *converted_provider_pkgname = NULL;
	int verified;

	if (aul_app_get_pkgname_bypid(pid, pid_pkgname, sizeof(pid_pkgname)) != AUL_R_OK) {
		if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
			DbgPrint("Debug mode is enabled: [%s] [%s]\n", abi, provider_pkgname);
			if (provider_pkgname) {
				strncpy(pid_pkgname, provider_pkgname, sizeof(pid_pkgname));
				DbgPrint("[%d]_pkgname is updated to [%s]\n", pid, pid_pkgname);
			} else {
				DbgPrint("There is no way to get the pkgname of %d even though we are in the debug mode\n", pid);
				return NULL;
			}
		} else {
			ErrPrint("pid[%d] is not authroized provider package\n", pid);
			return NULL;
		}
	}

	abi_pkgname = widget_abi_get_pkgname_by_abi(abi);
	if (!abi_pkgname) {
		ErrPrint("ABI has no valid entry[%s]\n", abi);
		return NULL;
	}

	if (!strcmp(abi, "meta")) {
		converted_provider_pkgname = package_meta_tag(provider_pkgname, abi_pkgname);
		if (!converted_provider_pkgname || strcmp(converted_provider_pkgname, pid_pkgname)) {
			DbgPrint("Meta tag is not matched: %s <> %s (%s)\n", converted_provider_pkgname, pid_pkgname, abi_pkgname);
			DbgFree(converted_provider_pkgname);
			return NULL;
		}

		DbgPrint("Meta package detected: %s\n", converted_provider_pkgname);
		return converted_provider_pkgname;
	}

	if (!provider_pkgname) {
		DbgPrint("provider_pkgname is NULL (%s)\n", pid_pkgname);
		return NULL;
	}

	if (!strcmp(abi_pkgname, pid_pkgname)) {
		/*!
		 * This request is comes from predefined service provider.
		 * In this case, we should believe its request.
		 */
		DbgPrint("Provider pkgname[%s]\n", provider_pkgname);
		return strdup(provider_pkgname);
	} else if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
		DbgPrint("Debug mode is enabled, use the %s as a widget_id\n", provider_pkgname);
		return strdup(provider_pkgname);
	}

	/*!
	 * This request is comes from standalone widget provider (not the predefined service provider)
	 */
	pkgid = package_get_pkgid(provider_pkgname);
	list_handle = widget_service_create_widget_list(pkgid, NULL);
	DbgFree(pkgid);

	verified = 0;
	widget_id = NULL;
	while (widget_service_get_item_from_widget_list(list_handle, NULL, &widget_id, NULL) == WIDGET_ERROR_NONE) {
		if (!widget_id) {
			ErrPrint("Invalid widget_id\n");
			continue;
		}

		// tmp == /APPID/.provider <<- PROVIDER UI-APP
		// widget_id == org.tizen.watch-hello <<-- WIDGET ID
		// provider_pkgname == org.tizen.watch-hello.provider
		converted_provider_pkgname = widget_util_replace_string(abi_pkgname, WIDGET_CONF_REPLACE_TAG_APPID, widget_id);
		if (!converted_provider_pkgname) {
			DbgFree(widget_id);
			widget_id = NULL;
			continue;
		}

		/* Verify the Package Name */
		verified = !strcmp(converted_provider_pkgname, provider_pkgname);
		DbgFree(converted_provider_pkgname);

		if (verified) {
			break;
		}

		DbgFree(widget_id);
		widget_id = NULL;
	}

	widget_service_destroy_widget_list(list_handle);
	return widget_id;
}

static Eina_Bool lazy_key_status_cb(void *data)
{
	struct event_cbdata *cbdata = data;

	if (instance_unref(cbdata->inst)) {
		instance_send_key_status(cbdata->inst, cbdata->status);
	} else {
		DbgPrint("Skip sending key status (%d)\n", cbdata->status);
	}
	/*!
	 * If instance_unref returns NULL,
	 * The instance is destroyed. it means, we don't need to send event to the viewer
	 */
	DbgFree(cbdata);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool lazy_access_status_cb(void *data)
{
	struct event_cbdata *cbdata = data;

	if (instance_unref(cbdata->inst)) {
		instance_send_access_status(cbdata->inst, cbdata->status);
	} else {
		DbgPrint("Skip sending access status (%d)\n", cbdata->status);
	}
	/*!
	 * If instance_unref returns NULL,
	 * The instance is destroyed. it means, we don't need to send event to the viewer
	 */
	DbgFree(cbdata);
	return ECORE_CALLBACK_CANCEL;
}

int send_delayed_key_status(struct inst_info *inst, int ret)
{
	struct event_cbdata *cbdata;

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ret = WIDGET_ERROR_OUT_OF_MEMORY;
	} else {
		cbdata->inst = instance_ref(inst);
		cbdata->status = ret;

		if (!ecore_timer_add(DELAY_TIME, lazy_key_status_cb, cbdata)) {
			(void)instance_unref(cbdata->inst);
			DbgFree(cbdata);
			ret = WIDGET_ERROR_FAULT;
		} else {
			ret = WIDGET_ERROR_NONE;
		}
	}

	return ret;
}

int send_delayed_access_status(struct inst_info *inst, int ret)
{
	struct event_cbdata *cbdata;

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ret = WIDGET_ERROR_OUT_OF_MEMORY;
	} else {
		cbdata->inst = instance_ref(inst);
		cbdata->status = ret;

		if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
			(void)instance_unref(cbdata->inst);
			DbgFree(cbdata);
			ret = WIDGET_ERROR_FAULT;
		} else {
			ret = WIDGET_ERROR_NONE;
		}
	}

	return ret;
}

static int forward_widget_event_packet(const struct pkg_info *pkg, struct inst_info *inst, const struct packet *packet)
{
	struct buffer_info *buffer;
	struct slave_node *slave;
	int ret;

	buffer = instance_widget_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	packet_ref((struct packet *)packet);
	ret = slave_rpc_request_only(slave, package_name(pkg), (struct packet *)packet, 0);

out:
	return ret;
}

static int forward_gbar_event_packet(const struct pkg_info *pkg, struct inst_info *inst, const struct packet *packet)
{
	struct buffer_info *buffer;
	struct slave_node *slave;
	int ret;

	buffer = instance_gbar_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	packet_ref((struct packet *)packet);
	ret = slave_rpc_request_only(slave, package_name(pkg), (struct packet *)packet, 0);

out:
	return ret;
}

static int forward_gbar_access_packet(const struct pkg_info *pkg, struct inst_info *inst, const char *command, double timestamp, struct access_info *event)
{
	int ret;
	struct buffer_info *buffer;
	struct slave_node *slave;
	struct packet *p;

	buffer = instance_gbar_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	p = packet_create_noack(command, "ssdiii", package_name(pkg), instance_id(inst), timestamp, event->x, event->y, event->type);
	ret = slave_rpc_request_only(slave, package_name(pkg), p, 0);

out:
	return ret;
}

static int forward_widget_access_packet(const struct pkg_info *pkg, struct inst_info *inst, const char *command, double timestamp, struct access_info *event)
{
	int ret;
	struct buffer_info *buffer;
	struct slave_node *slave;
	struct packet *p;

	buffer = instance_widget_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	p = packet_create_noack(command, "ssdiii", package_name(pkg), instance_id(inst), timestamp, event->x, event->y, event->type);
	ret = slave_rpc_request_only(slave, package_name(pkg), p, 0);

out:
	return ret;
}

static int forward_gbar_key_packet(const struct pkg_info *pkg, struct inst_info *inst, const char *command, double timestamp, unsigned int keycode)
{
	int ret;
	struct buffer_info *buffer;
	struct slave_node *slave;
	struct packet *p;

	buffer = instance_widget_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	p = packet_create_noack(command, "ssdi", package_name(pkg), instance_id(inst), timestamp, keycode);
	ret = slave_rpc_request_only(slave, package_name(pkg), p, 0);

out:
	return ret;
}

static int forward_widget_key_packet(const struct pkg_info *pkg, struct inst_info *inst, const char *command, double timestamp, unsigned int keycode)
{
	int ret;
	struct buffer_info *buffer;
	struct slave_node *slave;
	struct packet *p;

	buffer = instance_widget_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	p = packet_create_noack(command, "ssdi", package_name(pkg), instance_id(inst), timestamp, keycode);
	ret = slave_rpc_request_only(slave, package_name(pkg), p, 0);

out:
	return ret;
}

static int slave_fault_open_script_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)script_handler_unload(instance_gbar_script(data), 1);
	(void)instance_slave_close_gbar(data, instance_gbar_owner(data), WIDGET_CLOSE_GBAR_FAULT);
	(void)instance_client_gbar_created(data, WIDGET_ERROR_FAULT);

	timer = instance_del_data(data, LAZY_GBAR_OPEN_TAG);
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_open_buffer_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_slave_close_gbar(data, instance_gbar_owner(data), WIDGET_CLOSE_GBAR_FAULT);
	(void)instance_client_gbar_created(data, WIDGET_ERROR_FAULT);

	timer = instance_del_data(data, GBAR_OPEN_MONITOR_TAG);
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_close_script_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_client_gbar_destroyed(data, WIDGET_ERROR_FAULT);

	timer = instance_del_data(data, LAZY_GBAR_CLOSE_TAG);
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_close_buffer_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_client_gbar_destroyed(data, WIDGET_ERROR_FAULT);

	timer = instance_del_data(data, LAZY_GBAR_CLOSE_TAG);
	if (!timer) {
		timer = instance_del_data(data, GBAR_CLOSE_MONITOR_TAG);
	}

	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_resize_buffer_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_slave_close_gbar(data, instance_gbar_owner(data), WIDGET_CLOSE_GBAR_FAULT);
	(void)instance_client_gbar_destroyed(data, WIDGET_ERROR_FAULT);

	timer = instance_del_data(data, GBAR_RESIZE_MONITOR_TAG);
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int key_event_widget_route_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	struct slave_node *slave;
	struct packet *packet;
	unsigned int cmd;

	if (!inst) {
		DbgPrint("Instance is deleted.\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		cmd = CMD_WIDGET_KEY_DOWN;
		break;
	case EVENT_STATE_ACTIVATED:
		cmd = CMD_WIDGET_KEY_DOWN;
		break;
	case EVENT_STATE_DEACTIVATE:
		cmd = CMD_WIDGET_KEY_UP;
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssdii", package_name(pkg), instance_id(inst), event_info->tv, event_info->keycode, event_info->slot);
	if (!packet) {
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, package_name(pkg), packet, 0);
}

static int mouse_event_widget_route_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	struct slave_node *slave;
	struct packet *packet;
	unsigned int cmd;

	if (!inst) {
		DbgPrint("Instance is deleted.\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		cmd = CMD_WIDGET_MOUSE_DOWN;
		break;
	case EVENT_STATE_ACTIVATED:
		cmd = CMD_WIDGET_MOUSE_MOVE;
		break;
	case EVENT_STATE_DEACTIVATE:
		cmd = CMD_WIDGET_MOUSE_UP;
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssdiiiddi", package_name(pkg), instance_id(inst), event_info->tv, event_info->x, event_info->y, (int)event_info->source, event_info->ratio_w, event_info->ratio_h, event_info->slot);
	if (!packet) {
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, package_name(pkg), packet, 0);
}

static int key_event_widget_consume_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct script_info *script;
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	double timestamp;

	pkg = instance_package(inst);
	if (!pkg) {
		return 0;
	}

	script = instance_widget_script(inst);
	if (!script) {
		return WIDGET_ERROR_FAULT;
	}

	timestamp = event_info->tv;

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		script_handler_update_keycode(script, event_info->keycode);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_KEY_DOWN, timestamp);
		break;
	case EVENT_STATE_ACTIVATED:
		script_handler_update_keycode(script, event_info->keycode);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_KEY_DOWN, timestamp);
		break;
	case EVENT_STATE_DEACTIVATE:
		script_handler_update_keycode(script, event_info->keycode);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_UP, timestamp);
		break;
	default:
		ErrPrint("Unknown event\n");
		break;
	}

	return 0;
}

static int mouse_event_widget_consume_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct script_info *script;
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	double timestamp;

	pkg = instance_package(inst);
	if (!pkg) {
		return 0;
	}

	script = instance_widget_script(inst);
	if (!script) {
		return WIDGET_ERROR_FAULT;
	}

	timestamp = event_info->tv;

	/**
	 * @TODO
	 * Determines what will do for event coordinate.
	 * Whether it should be rotated or not.
	 */
	switch (state) {
	case EVENT_STATE_ACTIVATE:
		script_handler_update_pointer(script, event_info->slot, event_info->x * event_info->ratio_w, event_info->y * event_info->ratio_h, 1);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_DOWN, timestamp);
		break;
	case EVENT_STATE_ACTIVATED:
		script_handler_update_pointer(script, event_info->slot, event_info->x * event_info->ratio_w, event_info->y * event_info->ratio_h, -1);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_MOVE, timestamp);
		break;
	case EVENT_STATE_DEACTIVATE:
		script_handler_update_pointer(script, event_info->slot, event_info->x * event_info->ratio_w, event_info->y * event_info->ratio_h, 0);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_UP, timestamp);
		break;
	default:
		break;
	}

	return 0;
}

static int key_event_gbar_route_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	struct slave_node *slave;
	struct packet *packet;
	unsigned int cmd;

	if (!inst) {
		DbgPrint("Instance is deleted.\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		cmd = CMD_GBAR_KEY_DOWN;
		break;
	case EVENT_STATE_ACTIVATED:
		cmd = CMD_GBAR_KEY_DOWN;
		break;
	case EVENT_STATE_DEACTIVATE:
		cmd = CMD_GBAR_KEY_UP;
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssdi", package_name(pkg), instance_id(inst), event_info->tv, event_info->keycode);
	if (!packet) {
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, package_name(pkg), packet, 0);
}

static int mouse_event_gbar_route_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	struct slave_node *slave;
	struct packet *packet;
	unsigned int cmd;

	if (!inst) {
		DbgPrint("Instance is deleted.\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		cmd = CMD_GBAR_MOUSE_DOWN;
		break;
	case EVENT_STATE_ACTIVATED:
		cmd = CMD_GBAR_MOUSE_MOVE;
		break;
	case EVENT_STATE_DEACTIVATE:
		cmd = CMD_GBAR_MOUSE_UP;
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	packet = packet_create_noack((const char *)&cmd, "ssdiiiddi", package_name(pkg), instance_id(inst), event_info->tv, event_info->x, event_info->y, (int)event_info->source, event_info->ratio_w, event_info->ratio_h, event_info->slot);
	if (!packet) {
		return WIDGET_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, package_name(pkg), packet, 0);
}

static int key_event_gbar_consume_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct script_info *script;
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	double timestamp;

	pkg = instance_package(inst);
	if (!pkg) {
		return 0;
	}

	script = instance_gbar_script(inst);
	if (!script) {
		return WIDGET_ERROR_FAULT;
	}

	timestamp = event_info->tv;

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		script_handler_update_keycode(script, event_info->keycode);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_KEY_DOWN, timestamp);
		break;
	case EVENT_STATE_ACTIVATED:
		script_handler_update_keycode(script, event_info->keycode);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_KEY_DOWN, timestamp);
		break;
	case EVENT_STATE_DEACTIVATE:
		script_handler_update_keycode(script, event_info->keycode);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_KEY_UP, timestamp);
		break;
	default:
		ErrPrint("Unknown event\n");
		break;
	}

	return 0;
}

static int mouse_event_gbar_consume_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct script_info *script;
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	double timestamp;

	pkg = instance_package(inst);
	if (!pkg) {
		return 0;
	}

	script = instance_gbar_script(inst);
	if (!script) {
		return WIDGET_ERROR_FAULT;
	}

	timestamp = event_info->tv;

	/**
	 * @TODO
	 * Determines what will do for event coordinate.
	 * Whether it should be rotated or not.
	 */

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		script_handler_update_pointer(script, event_info->slot, event_info->x * event_info->ratio_w, event_info->y * event_info->ratio_h, 1);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_DOWN, timestamp);
		break;
	case EVENT_STATE_ACTIVATED:
		script_handler_update_pointer(script, event_info->slot, event_info->x * event_info->ratio_w, event_info->y * event_info->ratio_h, -1);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_MOVE, timestamp);
		break;
	case EVENT_STATE_DEACTIVATE:
		script_handler_update_pointer(script, event_info->slot, event_info->x * event_info->ratio_w, event_info->y * event_info->ratio_h, 0);
		(void)script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_UP, timestamp);
		break;
	default:
		break;
	}
	return 0;
}

static struct packet *client_acquire(pid_t pid, int handle, const struct packet *packet) /*!< timestamp, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *direct_addr;
	double timestamp;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (client) {
		ErrPrint("Client is already exists %d\n", pid);
		ret = WIDGET_ERROR_ALREADY_EXIST;
		goto out;
	}

	if (packet_get(packet, "ds", &timestamp, &direct_addr) != 2) {
		ErrPrint("Invalid arguemnt\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = 0;
	/*!
	 * \note
	 * client_create will invoke the client created callback
	 */
	client = client_create(pid, handle, direct_addr);
	if (!client) {
		ErrPrint("Failed to create a new client for %d\n", pid);
		ret = WIDGET_ERROR_FAULT;
	}

out:
	result = packet_create_reply(packet, "ii", ret, WIDGET_CONF_EXTRA_BUFFER_COUNT);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *cilent_release(pid_t pid, int handle, const struct packet *packet) /*!< pid, ret */
{
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	client_destroy(client);
	ret = 0;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static int validate_request(pid_t pid, struct slave_node *slave, const char *pkgname, const char *id, struct inst_info **out_inst, const struct pkg_info **out_pkg)
{
	struct inst_info *inst;
	const struct pkg_info *pkg;

	if (slave) {
		if (slave_valid(slave)) {
			inst = package_find_instance_by_id(pkgname, id);
		} else {
			ErrPrint("slave is not valid (%s)\n", id);
			return WIDGET_ERROR_INVALID_PARAMETER;
		}
	} else {
		inst = package_find_instance_by_id(pkgname, id);
	}

	if (!inst) {
		ErrPrint("Instance is not exists (%s)\n", id);
		return WIDGET_ERROR_NOT_EXIST;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("System error - instance has no package?\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (package_is_fault(pkg)) {
		ErrPrint("Faulted package: %s\n", package_name(pkg));
		return WIDGET_ERROR_FAULT;
	}

	if (out_inst) {
		*out_inst = inst;
	}

	if (out_pkg) {
		*out_pkg = pkg;
	}

	return WIDGET_ERROR_NONE;
}

/*!< pid, pkgname, filename, event, timestamp, x, y, ret */
static struct packet *client_clicked(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	const char *event;
	double timestamp;
	double x;
	double y;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sssddd", &pkgname, &id, &event, &timestamp, &x, &y);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		(void)instance_clicked(inst, event, timestamp, x, y);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_update_mode(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	int active_update;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &active_update);
	if (ret != 3) {
		ErrPrint("Invalid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		/*!
		 * \note
		 * Send change update mode request to a slave
		 */
		ret = instance_set_update_mode(inst, active_update);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

/* pid, pkgname, filename, signal_name, source, s, sy, ex, ey, ret */
static struct packet *client_text_signal(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *signal_name;
	const char *source;
	double sx;
	double sy;
	double ex;
	double ey;
	struct inst_info *inst = NULL;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssssdddd", &pkgname, &id, &signal_name, &source, &sx, &sy, &ex, &ey);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		ret = instance_text_signal_emit(inst, signal_name, source, sx, sy, ex, ey);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static Eina_Bool lazy_delete_cb(void *data)
{
	struct deleted_item *item = data;

	DbgPrint("Lazy delete callback called\n");
	/*!
	 * Before invoke this callback, the instance is able to already remove this client
	 * So check it again
	 */
	if (instance_has_client(item->inst, item->client)) {
		(void)instance_unicast_deleted_event(item->inst, item->client, WIDGET_ERROR_NONE);
		(void)instance_del_client(item->inst, item->client);
	}

	(void)client_unref(item->client);
	(void)instance_unref(item->inst);
	DbgFree(item);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *client_delete(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;
	int type;
	double timestamp;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssid", &pkgname, &id, &type, &timestamp);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}
	/*!
	 * \note
	 * Below two types must has to be sync'd with widget-viewer
	 *
	 * WIDGET_DELETE_PERMANENTLY = 0x01,
	 * WIDGET_DELETE_TEMPORARY = 0x02,
	 *
	 */

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		DbgPrint("Failed to find by id(%s), try to find it using timestamp(%lf)\n", id, timestamp);
		inst = package_find_instance_by_timestamp(pkgname, timestamp);
		if (!inst) {
			goto out;
		}

		pkg = instance_package(inst);
		if (!pkg) {
			ErrPrint("Package info is not valid: %s\n", id);
			goto out;
		}
	}

	if (package_is_fault(pkg)) {
		DbgPrint("Faulted package. will be deleted soon: %s\n", id);
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	if (instance_client(inst) != client) {
		if (instance_has_client(inst, client)) {
			struct deleted_item *item;

			item = malloc(sizeof(*item));
			if (!item) {
				ErrPrint("malloc: %d\n", errno);
				ret = WIDGET_ERROR_OUT_OF_MEMORY;
			} else {
				/*!
				 * \NOTE:
				 * Send DELETED EVENT to the client.
				 * after return from this function.
				 *
				 * Client will prepare the deleted event after get this function's return value.
				 * So We have to make a delay to send a deleted event.
				 */

				item->client = client_ref(client);
				item->inst = instance_ref(inst);

				if (!ecore_timer_add(DELAY_TIME, lazy_delete_cb, item)) {
					ErrPrint("Failed to add a delayzed delete callback\n");
					(void)client_unref(client);
					(void)instance_unref(inst);
					DbgFree(item);
					ret = WIDGET_ERROR_FAULT;
				} else {
					ret = WIDGET_ERROR_NONE;
				}
			}
		} else {
			ErrPrint("Client has no permission\n");
			ret = WIDGET_ERROR_PERMISSION_DENIED;
		}
	} else {
		switch (type) {
		case WIDGET_DELETE_PERMANENTLY:
			ret = instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			break;
		case WIDGET_DELETE_TEMPORARY:
			ret = instance_destroy(inst, WIDGET_DESTROY_TYPE_TEMPORARY);
			break;
		default:
			break;
		}
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_resize(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, w, h, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	struct inst_info *inst = NULL;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssii", &pkgname, &id, &w, &h);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("RESIZE: Client request resize to %dx%d (pid: %d, pkgname: %s)\n", w, h, pid, pkgname);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = WIDGET_ERROR_PERMISSION_DENIED;
	} else {
		ret = instance_resize(inst, w, h);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_new(pid_t pid, int handle, const struct packet *packet) /* pid, timestamp, pkgname, content, cluster, category, period, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *content;
	const char *cluster;
	const char *category;
	double period;
	double timestamp;
	int ret;
	struct pkg_info *info;
	int width;
	int height;
	char *widget_id;
	char *mainappid;
	int max_count;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "dssssdii", &timestamp, &pkgname, &content, &cluster, &category, &period, &width, &height);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("pid[%d] period[%lf] pkgname[%s] content[%s] cluster[%s] category[%s] period[%lf]\n",
			pid, timestamp, pkgname, content, cluster, category, period);

	widget_id = package_widget_pkgname(pkgname);
	if (!widget_id) {
		ErrPrint("This %s has no widget package\n", pkgname);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	/**
	 * @TODO
	 * Should be replaced with package manager API to decide whether it is enabled or not.
	 */
	mainappid = widget_service_get_main_app_id(widget_id);
	if (!package_is_enabled(mainappid)) {
		ErrPrint("%s is disabled\n", mainappid);
		DbgFree(mainappid);
		DbgFree(widget_id);
		ret = WIDGET_ERROR_DISABLED;
		goto out;
	}
	DbgFree(mainappid);
	mainappid = NULL;

	info = package_find(widget_id);
	if (!info) {
		char *pkgid;
		pkgid = widget_service_get_package_id(widget_id);
		if (!pkgid) {
			DbgFree(mainappid);
			DbgFree(widget_id);
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		info = package_create(pkgid, widget_id);
		DbgFree(pkgid);
	}

	max_count = widget_service_get_widget_max_count(widget_id);
	if (max_count < 0) {
		ErrPrint("Failed to get max_count[%s] = %d\n", widget_id, max_count);
		max_count = 0;
	}

	if (!info) {
		ret = WIDGET_ERROR_FAULT;
	} else if (package_is_fault(info)) {
		ret = WIDGET_ERROR_FAULT;
	} else if (util_free_space(WIDGET_CONF_IMAGE_PATH) <= WIDGET_CONF_MINIMUM_SPACE) {
		ErrPrint("Not enough space\n");
		ret = WIDGET_ERROR_FILE_NO_SPACE_ON_DEVICE;
	} else if (max_count && max_count <= package_instance_count(info, 1)) {
		ErrPrint("Reached to the max count of widgets %d, %d\n", max_count, package_instance_count(info, 1));
		ret = WIDGET_ERROR_CANCELED;
	} else {
		struct inst_info *inst;

		if (period > 0.0f && period < WIDGET_CONF_MINIMUM_PERIOD) {
			period = WIDGET_CONF_MINIMUM_PERIOD;
		}

		inst = instance_create(client, timestamp, widget_id, content, cluster, category, period, width, height);
		/*!
		 * \note
		 * Using the "inst" without validate its value is at my disposal. ;)
		 */
		ret = inst ? 0 : WIDGET_ERROR_FAULT;
	}

	DbgFree(widget_id);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_change_visibility(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	enum widget_visible_state state;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, (int *)&state);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = WIDGET_ERROR_PERMISSION_DENIED;
	} else {
		ret = instance_set_visible_state(inst, state);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_set_period(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, period, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	double period;
	int ret;
	struct inst_info *inst = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssd", &pkgname, &id, &period);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] period[%lf]\n", pid, pkgname, period);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = WIDGET_ERROR_PERMISSION_DENIED;
	} else {
		ret = instance_set_period(inst, period);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_change_group(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, cluster, category, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *cluster;
	const char *category;
	struct inst_info *inst = NULL;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssss", &pkgname, &id, &cluster, &category);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] cluster[%s] category[%s]\n", pid, pkgname, cluster, category);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a widget package name.
	 */
	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = WIDGET_ERROR_PERMISSION_DENIED;
	} else {
		ret = instance_change_group(inst, cluster, category);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_gbar_mouse_enter(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_IN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_leave(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_OUT, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_down(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, id, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, 1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_DOWN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_up(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, 0);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_UP, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_move(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_MOVE, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_move(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_MOVE, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static int inst_del_cb(struct inst_info *inst, void *data)
{
	int ret;

	/*
	 * If you deactivate the event thread,
	 * It will calls event callbacks.
	 * And the event callbacks will try to access the "inst"
	 */
	(void)event_deactivate(-1, data, inst);

	/* Reset callback data to prevent accessing inst from event callback */
	ret = event_reset_cbdata(data, inst, NULL);
	DbgPrint("Instance delete callback called: %s (%d)\n", instance_id(inst), ret);

	if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
		(void)slave_set_priority(package_slave(instance_package(inst)), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
	}

	return -1; /* Delete this callback */
}

static struct packet *client_gbar_key_set(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &keycode, &device);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = event_activate(device, 0, 0, 1.0f, 1.0f, key_event_gbar_route_cb, inst);
		if (ret == WIDGET_ERROR_NONE) {
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
			}
			if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_gbar_route_cb) <= 0) {
				instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_gbar_route_cb);
			}
		}
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		ret = event_activate(device, 0, 0, 1.0f, 1.0f, key_event_gbar_consume_cb, inst);
		if (ret == WIDGET_ERROR_NONE) {
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
			}
			if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_gbar_consume_cb) <= 0) {
				instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_gbar_consume_cb);
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_key_unset(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &keycode, &device);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = event_deactivate(device, key_event_gbar_route_cb, inst);
		if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
			(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
		}
		/*
		 * This delete callback will be removed when the instance will be destroyed.
		 if (ret == 0) {
		 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_gbar_route_cb);
		 }
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		ret = event_deactivate(device, key_event_gbar_consume_cb, inst);
		if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
			(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
		}
		/*
		 * This delete callback will be removed when the instance will be destroyed.
		 if (ret == 0) {
		 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_gbar_consume_cb);
		 }
		 */
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_key_set(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &keycode, &device);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = event_activate(device, 0, 0, 1.0f, 1.0f, key_event_widget_route_cb, inst);
		if (ret == WIDGET_ERROR_NONE) {
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
			}
			if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_widget_route_cb) <= 0) {
				instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_widget_route_cb);
			}
		}
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		ret = event_activate(device, 0, 0, 1.0f, 1.0f, key_event_widget_consume_cb, inst);
		if (ret == WIDGET_ERROR_NONE) {
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
			}
			if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_widget_consume_cb) <= 0) {
				instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_widget_consume_cb);
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_key_unset(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &keycode, &device);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = event_deactivate(device, key_event_widget_route_cb, inst);
		if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
			(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
		}
		/*
		 * This delete callback will be removed when the instance will be destroyed.
		 if (ret == 0) {
		 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_widget_route_cb);
		 }
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		ret = event_deactivate(device, key_event_widget_consume_cb, inst);
		if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
			(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
		}
		/*
		 * This delete callback will be removed when the instance will be destroyed.
		 if (ret == 0) {
		 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, key_event_widget_consume_cb);
		 }
		 */
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_mouse_set(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		if (package_direct_input(pkg) == 0 || packet_set_fd((struct packet *)packet, event_input_fd()) < 0) {
			ret = event_activate(device, x, y, ratio_w, ratio_h, mouse_event_widget_route_cb, inst);
			if (ret == WIDGET_ERROR_NONE) {
				if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
					(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
				}
				if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_widget_route_cb) <= 0) {
					instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_widget_route_cb);
				}
			}
		} else {
			struct slave_node *slave;

			DbgPrint("Direct input is enabled(set for %s:%d)\n", id, packet_fd(packet));
			slave = package_slave(pkg);
			if (slave) {
				packet_ref((struct packet *)packet);
				ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
			} else {
				ErrPrint("Unable to find a slave for %s\n", pkgname);
				ret = WIDGET_ERROR_FAULT;
			}
		}
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		ret = event_activate(device, x, y, ratio_w, ratio_h, mouse_event_widget_consume_cb, inst);
		if (ret == WIDGET_ERROR_NONE) {
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
			}
			if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_widget_consume_cb) <= 0) {
				instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_widget_consume_cb);
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}
out:
	return NULL;
}

static struct packet *client_widget_mouse_unset(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		if (package_direct_input(pkg) == 0) {
			/* Forcely update the X,Y position using viewer's */
			event_set_mouse_xy(device, x, y, ratio_w, ratio_h, timestamp);

			ret = event_deactivate(device, mouse_event_widget_route_cb, inst);
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
			}
			/*
			 * This delete callback will be removed when the instance will be destroyed.
			 if (ret == 0) {
				 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_widget_route_cb);
			 }
			 */
		} else {
			struct slave_node *slave;

			DbgPrint("Direct input is enabled(unset) for %s\n", id);
			slave = package_slave(pkg);
			if (slave) {
				packet_ref((struct packet *)packet);
				ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
			} else {
				ErrPrint("Unable to find a slave for %s\n", pkgname);
				ret = WIDGET_ERROR_FAULT;
			}
		}
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		/* Forcely update the X,Y position using viewer's */
		event_set_mouse_xy(device, x, y, ratio_w, ratio_h, timestamp);

		ret = event_deactivate(device, mouse_event_widget_consume_cb, inst);
		if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
			(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
		}
		/*
		 * This delete callback will be removed when the instance will be destroyed.
		 if (ret == 0) {
			 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_widget_consume_cb);
		 }
		 */
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}
out:
	return NULL;
}

static struct packet *client_gbar_mouse_set(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		if (package_direct_input(pkg) == 0 || packet_set_fd((struct packet *)packet, event_input_fd()) < 0) {
			ret = event_activate(device, x, y, ratio_w, ratio_h, mouse_event_gbar_route_cb, inst);
			if (ret == WIDGET_ERROR_NONE) {
				if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
					(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
				}
				if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_gbar_route_cb) <= 0) {
					instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_gbar_route_cb);
				}
			}
		} else {
			struct slave_node *slave;

			DbgPrint("Direct input is enabled(set for %s:%d)\n", id, packet_fd(packet));
			slave = package_slave(pkg);
			if (slave) {
				packet_ref((struct packet *)packet);
				ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
			} else {
				ErrPrint("Unable to find a slave for %s\n", pkgname);
				ret = WIDGET_ERROR_FAULT;
			}
		}
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		ret = event_activate(device, x, y, ratio_w, ratio_h, mouse_event_gbar_consume_cb, inst);
		if (ret == WIDGET_ERROR_NONE) {
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_ON);
			}
			if (instance_event_callback_is_added(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_gbar_consume_cb) <= 0) {
				instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_gbar_consume_cb);
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	return NULL;
}

static struct packet *client_widget_mouse_on_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_ON_SCROLL, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_off_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_OFF_SCROLL, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_on_hold(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_ON_HOLD, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_off_hold(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_OFF_HOLD, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_on_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_ON_SCROLL, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_off_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_OFF_SCROLL, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_on_hold(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_ON_HOLD, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_off_hold(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_event_packet(pkg, inst, packet);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_OFF_HOLD, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_mouse_unset(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		if (package_direct_input(pkg) == 0) {
			/* Forcely update the X,Y position using viewer's */
			event_set_mouse_xy(device, x, y, ratio_w, ratio_h, timestamp);

			ret = event_deactivate(device, mouse_event_gbar_route_cb, inst);
			if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
				(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
			}
			/*
			 * This delete callback will be removed when the instance will be destroyed.
			 if (ret == 0) {
				 instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_gbar_route_cb);
			 }
			 */
		} else {
			struct slave_node *slave;

			DbgPrint("Direct input is enabled(unset) for %s\n", id);
			slave = package_slave(pkg);
			if (slave) {
				packet_ref((struct packet *)packet);
				ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
			} else {
				ErrPrint("Unable to find a slave for %s\n", pkgname);
				ret = WIDGET_ERROR_FAULT;
			}
		}
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		/* Forcely update the X,Y position using viewer's */
		event_set_mouse_xy(device, x, y, ratio_w, ratio_h, timestamp);

		ret = event_deactivate(device, mouse_event_gbar_consume_cb, inst);
		if (WIDGET_CONF_SLAVE_EVENT_BOOST_OFF != WIDGET_CONF_SLAVE_EVENT_BOOST_ON) {
			(void)slave_set_priority(package_slave(pkg), WIDGET_CONF_SLAVE_EVENT_BOOST_OFF);
		}
		/*
		 * This delete callback will be removed when the instance will be destroyed.
		 if (ret == 0) {
			instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, mouse_event_gbar_consume_cb);
		 }
		 */
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}
out:
	return NULL;
}

static struct packet *client_widget_mouse_enter(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_IN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_leave(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, -1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_OUT, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_down(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, 1);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_DOWN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_widget_mouse_up(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int source;
	double ratio_w;
	double ratio_h;
	int device;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiiiddi", &pkgname, &id, &timestamp, &x, &y, &source, &ratio_w, &ratio_h, &device);
	if (ret != 9) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_event_packet(pkg, inst, packet);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, device, x * ratio_w, y * ratio_h, 0);
		script_handler_feed_event(script, WIDGET_SCRIPT_MOUSE_UP, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_gbar_access_action(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_ACTION, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_value_change(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_VALUE_CHANGE, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_mouse(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_MOUSE, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_back(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_BACK, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_over(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_OVER, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_read(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_READ, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_enable(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;
		int type;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		type = (event.type == 0) ? WIDGET_SCRIPT_ACCESS_DISABLE : WIDGET_SCRIPT_ACCESS_ENABLE;

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, type, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_hl(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;
		int type;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		switch (event.type) {
		case ACCESS_TYPE_CUR:
			type = WIDGET_SCRIPT_ACCESS_HIGHLIGHT;
			break;
		case ACCESS_TYPE_NEXT:
			type = WIDGET_SCRIPT_ACCESS_HIGHLIGHT_NEXT;
			break;
		case ACCESS_TYPE_PREV:
			type = WIDGET_SCRIPT_ACCESS_HIGHLIGHT_PREV;
			break;
		case ACCESS_TYPE_OFF:
			type = WIDGET_SCRIPT_ACCESS_UNHIGHLIGHT;
			break;
		default:
			ret = WIDGET_ERROR_INVALID_PARAMETER;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, type, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_access_activate(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_ACTIVATE, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_key_focus_in(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_FOCUS_IN, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_key_focus_out(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_FOCUS_OUT, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_key_down(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_DOWN, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pause_request(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	double timestamp;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is paused - manually reported\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (WIDGET_CONF_USE_XMONITOR) {
		DbgPrint("XMONITOR enabled. ignore client paused request\n");
	} else {
		xmonitor_pause(client);
	}

out:
	return NULL;
}

static struct packet *client_resume_request(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	double timestamp;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (WIDGET_CONF_USE_XMONITOR) {
		DbgPrint("XMONITOR enabled. ignore client resumed request\n");
	} else {
		xmonitor_resume(client);
	}

out:
	return NULL;
}

static struct packet *client_change_orientation(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	double timestamp;
	int degree;
	int ret;
	struct pkg_info *pkg;
	Eina_List *pkg_list;
	Eina_List *l;
	Eina_List *inst_list;
	Eina_List *inst_l;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		goto out;
	}

	ret = packet_get(packet, "di", &timestamp, &degree);
	if (ret != 2) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	ret = 0;

	client_set_orientation(client, degree);

	pkg_list = (Eina_List *)package_list();

	EINA_LIST_FOREACH(pkg_list, l, pkg) {
		inst_list = package_instance_list(pkg);
		EINA_LIST_FOREACH(inst_list, inst_l, inst) {
			if (instance_client(inst) == client || instance_has_client(inst, client)) {
				instance_set_orientation(inst, degree);
				ret++;
			}
		}
	}
	DbgPrint("%d instances are affected (orientation: %d)\n", ret, degree);
out:
	return NULL;
}

static struct packet *client_gbar_key_up(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Invalid parameter\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		ret = forward_gbar_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_gbar_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_UP, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_hl(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;
		int type;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		switch (event.type) {
		case ACCESS_TYPE_CUR:
			type = WIDGET_SCRIPT_ACCESS_HIGHLIGHT;
			break;
		case ACCESS_TYPE_NEXT:
			type = WIDGET_SCRIPT_ACCESS_HIGHLIGHT_NEXT;
			break;
		case ACCESS_TYPE_PREV:
			type = WIDGET_SCRIPT_ACCESS_HIGHLIGHT_PREV;
			break;
		case ACCESS_TYPE_OFF:
			type = WIDGET_SCRIPT_ACCESS_UNHIGHLIGHT;
			break;
		default:
			ret = WIDGET_ERROR_INVALID_PARAMETER;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, type, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_action(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_ACTION, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_value_change(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_VALUE_CHANGE, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_mouse(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_MOUSE, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_back(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_BACK, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_over(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_OVER, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_read(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_READ, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_enable(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	struct access_info event;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;
		int type;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		type = (event.type == 0) ? WIDGET_SCRIPT_ACCESS_DISABLE : WIDGET_SCRIPT_ACCESS_ENABLE;

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, type, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_access_activate(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct access_info event;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdiii", &pkgname, &id, &timestamp, &event.x, &event.y, &event.type);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_access_packet(pkg, inst, packet_command(packet), timestamp, &event);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, 0, event.x, event.y, event.type);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_ACCESS_ACTIVATE, timestamp);
		if (ret >= 0) {
			ret = send_delayed_access_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_key_down(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_DOWN, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_key_focus_in(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_FOCUS_IN, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_key_focus_out(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_FOCUS_OUT, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_key_up(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	unsigned int keycode;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	struct packet *result;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdi", &pkgname, &id, &timestamp, &keycode);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = forward_widget_key_packet(pkg, inst, packet_command(packet), timestamp, keycode);
	} else if (package_widget_type(pkg) == WIDGET_TYPE_SCRIPT) {
		struct script_info *script;

		script = instance_widget_script(inst);
		if (!script) {
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		script_handler_update_keycode(script, keycode);
		ret = script_handler_feed_event(script, WIDGET_SCRIPT_KEY_UP, timestamp);
		if (ret >= 0) {
			ret = send_delayed_key_status(inst, ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static int release_pixmap_cb(struct client_node *client, void *canvas)
{
	DbgPrint("Forcely unref the \"buffer\"\n");
	buffer_handler_pixmap_unref(canvas);
	return -1; /* Delete this callback */
}

static struct packet *client_widget_acquire_xpixmap(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int ret;
	int pixmap = 0;
	void *buf_ptr;
	struct buffer_info *buffer;
	int idx;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &idx);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (idx >= WIDGET_CONF_EXTRA_BUFFER_COUNT || idx < 0) {
		DbgPrint("Index is not valid: %d\n", idx);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	buffer = instance_widget_extra_buffer(inst, idx);
	if (!buffer) {
		ErrPrint("Extra buffer for %d is not available\n", idx);
		goto out;
	}

	buf_ptr = buffer_handler_pixmap_ref(buffer);
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		ErrPrint("Failed to add a new client deactivate callback\n");
		buffer_handler_pixmap_unref(buf_ptr);
	} else {
		pixmap = buffer_handler_pixmap(buffer);
		ret = WIDGET_ERROR_NONE;
	}

out:
	result = packet_create_reply(packet, "ii", pixmap, ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_acquire_pixmap(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int ret;
	int pixmap = 0;
	void *buf_ptr;
	struct buffer_info *buffer;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	buffer = instance_widget_buffer(inst);
	if (!buffer) {
		struct script_info *script_info;

		script_info = instance_widget_script(inst);
		if (!script_info) {
			ErrPrint("Unable to get WIDGET buffer: %s\n", id);
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		buffer = script_handler_buffer_info(script_info);
		if (!buffer) {
			ErrPrint("Unable to get buffer_info: %s\n", id);
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}
	}

	buf_ptr = buffer_handler_pixmap_ref(buffer);
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		ErrPrint("Failed to add a new client deactivate callback\n");
		buffer_handler_pixmap_unref(buf_ptr);
	} else {
		pixmap = buffer_handler_pixmap(buffer);
		ret = WIDGET_ERROR_NONE;
	}

out:
	result = packet_create_reply(packet, "ii", pixmap, ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_widget_release_pixmap(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	const char *id;
	struct client_node *client;
	int pixmap;
	void *buf_ptr;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pixmap);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, NULL, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		DbgPrint("It seems that the instance is already deleted: %s\n", id);
	}

	buf_ptr = buffer_handler_pixmap_find(pixmap);
	if (!buf_ptr) {
		ErrPrint("Failed to find a buf_ptr of 0x%X\n", pixmap);
		goto out;
	}

	if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr) == 0) {
		buffer_handler_pixmap_unref(buf_ptr);
	}

out:
	/**
	 * @note No reply packet
	 */
	return NULL;
}

static struct packet *client_gbar_acquire_xpixmap(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int ret;
	int pixmap = 0;
	void *buf_ptr;
	struct buffer_info *buffer;
	int idx;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &idx);
	if (ret != 3) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	if (idx >= WIDGET_CONF_EXTRA_BUFFER_COUNT || idx < 0) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	buffer = instance_gbar_extra_buffer(inst, idx);
	if (!buffer) {
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	buf_ptr = buffer_handler_pixmap_ref(buffer);
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		ErrPrint("Failed to add a new client deactivate callback\n");
		buffer_handler_pixmap_unref(buf_ptr);
	} else {
		pixmap = buffer_handler_pixmap(buffer);
		ret = WIDGET_ERROR_NONE;
	}

out:
	result = packet_create_reply(packet, "ii", pixmap, ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_acquire_pixmap(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int ret;
	int pixmap = 0;
	void *buf_ptr;
	struct buffer_info *buffer;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_get_data(inst, GBAR_RESIZE_MONITOR_TAG)) {
		ret = WIDGET_ERROR_RESOURCE_BUSY;
		goto out;
	}

	buffer = instance_gbar_buffer(inst);
	if (!buffer) {
		struct script_info *script_info;

		script_info = instance_gbar_script(inst);
		if (!script_info) {
			ErrPrint("Unable to get WIDGET buffer: %s\n", id);
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		buffer = script_handler_buffer_info(script_info);
		if (!buffer) {
			ErrPrint("Unable to get buffer_info: %s\n", id);
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}
	}

	buf_ptr = buffer_handler_pixmap_ref(buffer);
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		ErrPrint("Failed to add a new client deactivate callback\n");
		buffer_handler_pixmap_unref(buf_ptr);
	} else {
		pixmap = buffer_handler_pixmap(buffer);
		ret = WIDGET_ERROR_NONE;
	}

out:
	result = packet_create_reply(packet, "ii", pixmap, ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_gbar_release_pixmap(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	const char *id;
	struct client_node *client;
	int pixmap;
	void *buf_ptr;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pixmap);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, NULL, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		DbgPrint("It seems that the instance is already deleted: %s\n", id);
	}

	buf_ptr = buffer_handler_pixmap_find(pixmap);
	if (!buf_ptr) {
		ErrPrint("Failed to find a buf_ptr of 0x%X\n", pixmap);
		goto out;
	}

	if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr) == 0) {
		buffer_handler_pixmap_unref(buf_ptr);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pinup_changed(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, pinup, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int pinup;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		pinup = 0;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pinup);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		pinup = 0;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		ret = instance_set_pinup(inst, pinup);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static Eina_Bool lazy_gbar_created_cb(void *inst)
{
	struct pkg_info *pkg;

	if (!instance_del_data(inst, LAZY_GBAR_OPEN_TAG)) {
		ErrPrint("lazy,pd,open is already deleted.\n");
		return ECORE_CALLBACK_CANCEL;
	}

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;

		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_script_cb, inst);
		}
	}

	/*!
	 * After unref instance first,
	 * if the instance is not destroyed, try to notify the created GBAR event to the client.
	 */
	if (instance_unref(inst)) {
		int ret;
		ret = instance_client_gbar_created(inst, WIDGET_ERROR_NONE);
		if (ret < 0) {
			DbgPrint("Send GBAR Create event (%d) to client\n", ret);
		}
	}

	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool lazy_gbar_destroyed_cb(void *inst)
{
	struct pkg_info *pkg;
	struct slave_node *slave;

	if (!instance_del_data(inst, LAZY_GBAR_CLOSE_TAG)) {
		ErrPrint("lazy,pd,close is already deleted.\n");
		return ECORE_CALLBACK_CANCEL;
	}

	pkg = instance_package(inst);
	if (pkg) {
		slave = package_slave(pkg);
		if (slave) {
			if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
				DbgPrint("Delete script type close callback\n");
				(void)slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_script_cb, inst);
			} else if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
				DbgPrint("Delete buffer type close callback\n");
				(void)slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
			}
		}
	}

	if (instance_unref(inst)) {
		int ret;

		/*!
		 * If the instance is not deleted, we should send pd-destroy event from here.
		 */
		ret = instance_client_gbar_destroyed(inst, WIDGET_ERROR_NONE);
		if (ret < 0) {
			ErrPrint("Failed sending GBAR Destroy event (%d)\n", ret);
		}
	}

	return ECORE_CALLBACK_CANCEL;
}

static struct packet *client_gbar_move(pid_t pid, int handle, const struct packet *packet) /* pkgname, id, x, y */
{
	struct client_node *client;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	double x = 0.0f;
	double y = 0.0f;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdd", &pkgname, &id, &x, &y);
	if (ret != 4) {
		ErrPrint("Parameter is not correct\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		instance_slave_set_gbar_pos(inst, x, y);
		ret = instance_signal_emit(inst, "pd,move", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		int ix;
		int iy;

		instance_slave_set_gbar_pos(inst, x, y);
		ix = x * instance_gbar_width(inst);
		iy = y * instance_gbar_height(inst);
		script_handler_update_pointer(instance_gbar_script(inst), 0, ix, iy, 0);
		ret = instance_signal_emit(inst, "pd,move", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
	} else {
		ErrPrint("Invalid GBAR type\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}
out:
	DbgPrint("Update GBAR position: %d\n", ret);
	return NULL;
}

static Eina_Bool gbar_open_monitor_cb(void *inst)
{
	struct pkg_info *pkg;

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;

		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst);
		}
	}

	(void)instance_slave_close_gbar(inst, instance_gbar_owner(inst), WIDGET_CLOSE_GBAR_TIMEOUT);
	(void)instance_client_gbar_created(inst, WIDGET_ERROR_TIMED_OUT);
	(void)instance_del_data(inst, GBAR_OPEN_MONITOR_TAG);
	(void)instance_unref(inst);
	ErrPrint("GBAR Open request is timed-out (%lf)\n", WIDGET_CONF_GBAR_REQUEST_TIMEOUT);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool gbar_close_monitor_cb(void *inst)
{
	struct pkg_info *pkg;

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;

		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
		}
	}

	(void)instance_client_gbar_destroyed(inst, WIDGET_ERROR_TIMED_OUT);
	(void)instance_del_data(inst, GBAR_CLOSE_MONITOR_TAG);
	(void)instance_unref(inst);
	ErrPrint("GBAR Close request is not processed in %lf seconds\n", WIDGET_CONF_GBAR_REQUEST_TIMEOUT);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool gbar_resize_monitor_cb(void *inst)
{
	struct pkg_info *pkg;

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;
		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst);
		}
	}

	(void)instance_slave_close_gbar(inst, instance_gbar_owner(inst), WIDGET_CLOSE_GBAR_TIMEOUT);
	(void)instance_client_gbar_destroyed(inst, WIDGET_ERROR_TIMED_OUT);
	(void)instance_del_data(inst, GBAR_RESIZE_MONITOR_TAG);
	(void)instance_unref(inst);
	ErrPrint("GBAR Resize request is not processed in %lf seconds\n", WIDGET_CONF_GBAR_REQUEST_TIMEOUT);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *client_create_gbar(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	Ecore_Timer *gbar_monitor;
	double x;
	double y;

	DbgPrint("PERF_WIDGET\n");

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdd", &pkgname, &id, &x, &y);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_gbar_owner(inst)) {
		ErrPrint("GBAR is already owned\n");
		ret = WIDGET_ERROR_ALREADY_EXIST;
	} else if (package_gbar_type(instance_package(inst)) == GBAR_TYPE_BUFFER) {
		gbar_monitor = instance_get_data(inst, LAZY_GBAR_CLOSE_TAG);
		if (gbar_monitor) {
			ecore_timer_del(gbar_monitor);
			/* This timer attribute will be deleted */
			lazy_gbar_destroyed_cb(inst);
		}

		if (instance_get_data(inst, GBAR_OPEN_MONITOR_TAG)) {
			DbgPrint("GBAR Open request is already processed\n");
			ret = WIDGET_ERROR_ALREADY_EXIST;
			goto out;
		}

		if (instance_get_data(inst, GBAR_CLOSE_MONITOR_TAG)) {
			DbgPrint("GBAR Close request is already in process\n");
			ret = WIDGET_ERROR_RESOURCE_BUSY;
			goto out;
		}

		if (instance_get_data(inst, GBAR_RESIZE_MONITOR_TAG)) {
			DbgPrint("GBAR resize request is already in process\n");
			ret = WIDGET_ERROR_RESOURCE_BUSY;
			goto out;
		}

		instance_slave_set_gbar_pos(inst, x, y);
		/*!
		 * \note
		 * Send request to the slave.
		 * The SLAVE must has to repsonse this via "release_buffer" method.
		 */
		ret = instance_slave_open_gbar(inst, client);
		if (ret == (int)WIDGET_ERROR_NONE) {
			ret = instance_signal_emit(inst, "gbar,show", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
			if (ret != WIDGET_ERROR_NONE) {
				int tmp_ret;

				tmp_ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_FAULT);
				if (tmp_ret < 0) {
					ErrPrint("Unable to send script event for openning GBAR [%s], %d\n", pkgname, tmp_ret);
				}
			} else {
				gbar_monitor = ecore_timer_add(WIDGET_CONF_GBAR_REQUEST_TIMEOUT, gbar_open_monitor_cb, instance_ref(inst));
				if (!gbar_monitor) {
					(void)instance_unref(inst);
					ErrPrint("Failed to create a timer for GBAR Open monitor\n");
				} else {
					struct slave_node *slave;

					(void)instance_set_data(inst, GBAR_OPEN_MONITOR_TAG, gbar_monitor);

					slave = package_slave(pkg);
					if (!slave) {
						ErrPrint("Failed to get slave(%s)\n", pkgname);
						goto out;
					}

					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst) != WIDGET_ERROR_NONE) {
						ErrPrint("Failed to add fault handler: %s\n");
					}
				}
			}
		} else {
			ErrPrint("Unable to send request for openning GBAR [%s]\n", pkgname);
		}

		/*!
		 * \note
		 * GBAR craeted event will be send by the acquire_buffer function.
		 * Because the slave will make request the acquire_buffer to
		 * render the GBAR
		 *
		 * instance_client_gbar_created(inst);
		 */
	} else if (package_gbar_type(instance_package(inst)) == GBAR_TYPE_SCRIPT) {
		int ix;
		int iy;

		gbar_monitor = instance_get_data(inst, LAZY_GBAR_CLOSE_TAG);
		if (gbar_monitor) {
			ecore_timer_del(gbar_monitor);
			/* lazy,pd,close will be deleted */
			lazy_gbar_destroyed_cb(inst);
		}

		/*!
		 * \note
		 * ret value should be cared but in this case,
		 * we ignore this for this moment, so we have to handle this error later.
		 *
		 * if ret is less than 0, the slave has some problem.
		 * but the script mode doesn't need slave for rendering default view of GBAR
		 * so we can hanle it later.
		 */
		instance_slave_set_gbar_pos(inst, x, y);
		ix = x * instance_gbar_width(inst);
		iy = y * instance_gbar_height(inst);

		script_handler_update_pointer(instance_gbar_script(inst), 0, ix, iy, 0);

		ret = instance_slave_open_gbar(inst, client);
		if (ret == (int)WIDGET_ERROR_NONE) {
			ret = script_handler_load(instance_gbar_script(inst), 1);

			/*!
			 * \note
			 * Send the GBAR created event to the clients,
			 */
			if (ret == (int)WIDGET_ERROR_NONE) {

				/*!
				 * \note
				 * But the created event has to be send afte return
				 * from this function or the viewer couldn't care
				 * the event correctly.
				 */
				inst = instance_ref(inst); /* To guarantee the inst */

				/*!
				 * \note
				 * At here, we don't need to rememeber the timer object.
				 * Even if the timer callback is called, after the instance is destroyed.
				 * lazy_gbar_created_cb will decrease the instance refcnt first.
				 * At that time, if the instance is released, the timer callback will do nothing.
				 *
				 * 13-05-28
				 * I change my mind. There is no requirements to keep the timer handler.
				 * But I just add it to the tagged-data of the instance.
				 * Just reserve for future-use.
				 */
				gbar_monitor = ecore_timer_add(DELAY_TIME, lazy_gbar_created_cb, inst);
				if (!gbar_monitor) {
					ret = script_handler_unload(instance_gbar_script(inst), 1);
					ErrPrint("Unload script: %d\n", ret);

					ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_NORMAL);
					ErrPrint("Close GBAR %d\n", ret);

					inst = instance_unref(inst);
					if (!inst) {
						DbgPrint("Instance destroyed\n");
					}

					ErrPrint("Instance: %s\n", pkgname);

					ret = WIDGET_ERROR_FAULT;
				} else {
					struct slave_node *slave;

					(void)instance_set_data(inst, LAZY_GBAR_OPEN_TAG, gbar_monitor);

					slave = package_slave(pkg);
					if (!slave) {
						ErrPrint("Failed to get slave: %s\n", pkgname);
						goto out;
					}

					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_script_cb, inst) != WIDGET_ERROR_NONE) {
						ErrPrint("Failed to add fault callback: %s\n", pkgname);
					}
				}
			} else {
				int tmp_ret;
				tmp_ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_FAULT);
				if (tmp_ret < 0) {
					ErrPrint("Unable to load script: %d, (close: %d)\n", ret, tmp_ret);
				}
			}
		} else {
			ErrPrint("Unable open GBAR(%s): %d\n", pkgname, ret);
		}
	} else {
		ErrPrint("Invalid GBAR TYPE\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_destroy_gbar(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	Ecore_Timer *gbar_monitor;
	struct slave_node *slave;

	DbgPrint("PERF_WIDGET\n");

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (instance_gbar_owner(inst) != client) {
		if (instance_gbar_owner(inst) == NULL) {
			ErrPrint("GBAR looks already closed\n");
			ret = WIDGET_ERROR_ALREADY_STARTED;
		} else {
			ErrPrint("GBAR owner mimatched\n");
			ret = WIDGET_ERROR_PERMISSION_DENIED;
		}
	} else if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		DbgPrint("Buffer type GBAR\n");
		gbar_monitor = instance_del_data(inst, GBAR_OPEN_MONITOR_TAG);
		if (gbar_monitor) {
			ErrPrint("GBAR Open request is found. cancel it [%s]\n", pkgname);

			if (slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst) < 0) {
				DbgPrint("Failed to delete a deactivate callback\n");
			}

			/*!
			 * \note
			 * We should return negative value
			 * Or we have to send "destroyed" event to the client.
			 * If we didn't send destroyed event after return SUCCESS from here,
			 * The client will permanently waiting destroyed event.
			 * Because they understand that the destroy request is successfully processed.
			 */
			ret = instance_client_gbar_created(inst, WIDGET_ERROR_CANCELED);
			if (ret < 0) {
				ErrPrint("GBAR client create event: %d\n", ret);
			}

			ret = instance_client_gbar_destroyed(inst, WIDGET_ERROR_NONE);
			if (ret < 0) {
				ErrPrint("GBAR client destroy event: %d\n", ret);
			}

			ret = instance_signal_emit(inst, "gbar,hide", instance_id(inst), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
			if (ret < 0) {
				ErrPrint("GBAR close signal emit failed: %d\n", ret);
			}

			ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_NORMAL);
			if (ret < 0) {
				ErrPrint("GBAR close request failed: %d\n", ret);
			}

			ecore_timer_del(gbar_monitor);
			inst = instance_unref(inst);
			if (!inst) {
				DbgPrint("Instance is deleted\n");
			}
		} else if (instance_get_data(inst, LAZY_GBAR_CLOSE_TAG) || instance_get_data(inst, GBAR_CLOSE_MONITOR_TAG)) {
			DbgPrint("Close monitor is already fired\n");
			ret = WIDGET_ERROR_ALREADY_STARTED;
		} else {
			int resize_aborted = 0;

			gbar_monitor = instance_del_data(inst, GBAR_RESIZE_MONITOR_TAG);
			if (gbar_monitor) {
				ErrPrint("GBAR Resize request is found. clear it [%s]\n", pkgname);
				if (slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst) < 0) {
					DbgPrint("Failed to delete a deactivate callback\n");
				}

				ecore_timer_del(gbar_monitor);

				inst = instance_unref(inst);
				if (!inst) {
					DbgPrint("Instance is destroyed while resizing\n");
					goto out;
				}

				resize_aborted = 1;
			}

			ret = instance_signal_emit(inst, "gbar,hide", instance_id(inst), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
			if (ret < 0) {
				ErrPrint("GBAR close signal emit failed: %d\n", ret);
			}

			ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_NORMAL);
			if (ret < 0) {
				ErrPrint("GBAR close request failed: %d\n", ret);
			} else if (resize_aborted) {
				gbar_monitor = ecore_timer_add(DELAY_TIME, lazy_gbar_destroyed_cb, instance_ref(inst));
				if (!gbar_monitor) {
					ErrPrint("Failed to create a timer: %s\n", pkgname);
					inst = instance_unref(inst);
					if (!inst) {
						DbgPrint("Instance is deleted\n");
					}
				} else {
					DbgPrint("Resize is aborted\n");
					(void)instance_set_data(inst, LAZY_GBAR_CLOSE_TAG, gbar_monitor);
					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst) < 0) {
						ErrPrint("Failed to add a slave event callback\n");
					}
				}
			} else {
				gbar_monitor = ecore_timer_add(WIDGET_CONF_GBAR_REQUEST_TIMEOUT, gbar_close_monitor_cb, instance_ref(inst));
				if (!gbar_monitor) {
					ErrPrint("Failed to add pd close monitor\n");
					inst = instance_unref(inst);
					if (!inst) {
						ErrPrint("Instance is deleted while closing GBAR\n");
					}
				} else {
					DbgPrint("Add close monitor\n");
					(void)instance_set_data(inst, GBAR_CLOSE_MONITOR_TAG, gbar_monitor);
					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst) < 0) {
						ErrPrint("Failed to add SLAVE EVENT callback\n");
					}
				}
			}

			/*!
			 * \note
			 * release_buffer will be called by the slave after this routine.
			 * It will send the "gbar_destroyed" event to the client
			 *
			 * instance_client_gbar_destroyed(inst, WIDGET_ERROR_NONE);
			 *
			 * Or the "gbar_close_monitor_cb" or "lazy_gbar_destroyed_cb" will be called.
			 */
		}
	} else if (package_gbar_type(pkg) == GBAR_TYPE_SCRIPT) {
		DbgPrint("Script TYPE GBAR\n");
		gbar_monitor = instance_get_data(inst, LAZY_GBAR_OPEN_TAG);
		if (gbar_monitor) {
			ecore_timer_del(gbar_monitor);
			(void)lazy_gbar_created_cb(inst);
		}

		ret = script_handler_unload(instance_gbar_script(inst), 1);
		if (ret < 0) {
			ErrPrint("Unable to unload the script: %s, %d\n", pkgname, ret);
		}

		/*!
		 * \note
		 * Send request to the slave.
		 * The SLAVE must has to repsonse this via "release_buffer" method.
		 */
		ret = instance_slave_close_gbar(inst, client, WIDGET_CLOSE_GBAR_NORMAL);
		if (ret < 0) {
			ErrPrint("Unable to close the GBAR: %s, %d\n", pkgname, ret);
		}

		/*!
		 * \note
		 * Send the destroyed GBAR event to the client
		 */
		if (ret == (int)WIDGET_ERROR_NONE) {
			/*!
			 * \note
			 * 13-05-28
			 * I've changed my mind. There is no requirements to keep the timer handler.
			 * But I just add it to the tagged-data of the instance.
			 * Just reserve for future-use.
			 */
			DbgPrint("Add lazy GBAR destroy timer\n");
			gbar_monitor = ecore_timer_add(DELAY_TIME, lazy_gbar_destroyed_cb, instance_ref(inst));
			if (!gbar_monitor) {
				ErrPrint("Failed to create a timer: %s\n", pkgname);
				inst = instance_unref(inst);
				if (!inst) {
					DbgPrint("instance is deleted\n");
				}
			} else {
				(void)instance_set_data(inst, LAZY_GBAR_CLOSE_TAG, gbar_monitor);
				if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_script_cb, inst) < 0) {
					ErrPrint("Failed to add a event callback for slave\n");
				}
			}
		}
	} else {
		ErrPrint("Invalid GBAR TYPE\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_activate_package(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	int ret;
	struct pkg_info *info;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		pkgname = "";
		goto out;
	}

	ret = packet_get(packet, "s", &pkgname);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		pkgname = "";
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s]\n", pid, pkgname);

	/*!
	 * \NOTE:
	 * Validate the widget package name.
	 */
	if (!package_is_widget_pkgname(pkgname)) {
		ErrPrint("%s is not a valid widget package\n", pkgname);
		pkgname = "";
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	info = package_find(pkgname);
	if (!info) {
		ret = WIDGET_ERROR_NOT_EXIST;
	} else {
		ret = package_clear_fault(info);
	}

out:
	result = packet_create_reply(packet, "is", ret, pkgname);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_subscribed_group(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster, category);
	if (!strlen(cluster) || !strcasecmp(cluster, WIDGET_CONF_DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name\n");
		goto out;
	}

	/*!
	 * \todo
	 * SUBSCRIBE cluster & sub-cluster for a client.
	 */
	ret = client_subscribe_group(client, cluster, category);
	if (ret == 0) {
		package_alter_instances_to_client(client, ALTER_CREATE);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_delete_cluster(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "s", &cluster);
	if (ret != 1) {
		ErrPrint("Invalid parameters\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("pid[%d] cluster[%s]\n", pid, cluster);

	if (!strlen(cluster) || !strcasecmp(cluster, WIDGET_CONF_DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster: %s\n", cluster);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = WIDGET_ERROR_NOT_SUPPORTED;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static inline int update_pkg_cb(struct category *category, const char *pkgname, int force)
{
	const char *c_name;
	const char *s_name;

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid\n");
		return EXIT_FAILURE;
	}

	DbgPrint("Send refresh request: %s (%s/%s)\n", pkgname, c_name, s_name);
	slave_rpc_request_update(pkgname, "", c_name, s_name, NULL, force);

	/* Just try to create a new package */
	if (util_free_space(WIDGET_CONF_IMAGE_PATH) > WIDGET_CONF_MINIMUM_SPACE) {
		double timestamp;
		struct inst_info *inst;

		timestamp = util_timestamp();
		/*!
		 * \NOTE
		 * Don't need to check the subscribed clients.
		 * Because this callback is called by the requests of clients.
		 * It means. some clients wants to handle this instances ;)
		 */
		inst = instance_create(NULL, timestamp, pkgname, "", c_name, s_name, WIDGET_CONF_DEFAULT_PERIOD, 0, 0);
		if (!inst) {
			ErrPrint("Failed to create a new instance\n");
		}
	}

	return EXIT_SUCCESS;
}

static struct packet *client_update(pid_t pid, int handle, const struct packet *packet)
{
	struct inst_info *inst;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int force;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &force);
	if (ret != 3) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_client(inst) != client) {
		/* PERMISSIONS */
		ErrPrint("Insufficient permissions [%s] - %d\n", pkgname, pid);
	} else {
		slave_rpc_request_update(pkgname, id, instance_cluster(inst), instance_category(inst), NULL, force);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_refresh_group(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster_id;
	const char *category_id;
	struct client_node *client;
	int ret;
	struct cluster *cluster;
	struct category *category;
	struct context_info *info;
	Eina_List *info_list;
	Eina_List *l;
	int force;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &cluster_id, &category_id, &force);
	if (ret != 3) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster_id, category_id);

	if (!strlen(cluster_id) || !strcasecmp(cluster_id, WIDGET_CONF_DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name: %s\n", cluster_id);
		goto out;
	}

	cluster = group_find_cluster(cluster_id);
	if (!cluster) {
		ErrPrint("Cluster [%s] is not registered\n", cluster_id);
		goto out;
	}

	category = group_find_category(cluster, category_id);
	if (!category) {
		ErrPrint("Category [%s] is not registered\n", category_id);
		goto out;
	}

	info_list = group_context_info_list(category);
	EINA_LIST_FOREACH(info_list, l, info) {
		update_pkg_cb(category, group_pkgname_from_context_info(info), force);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_delete_category(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid paramenters\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("pid[%d] cluster[%s] category[%s]\n", pid, cluster, category);
	if (!strlen(cluster) || !strcasecmp(cluster, WIDGET_CONF_DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster: %s\n", cluster);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = WIDGET_ERROR_NOT_SUPPORTED;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_unsubscribed_group(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster, category);

	if (!strlen(cluster) || !strcasecmp(cluster, WIDGET_CONF_DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name: %s\n", cluster);
		goto out;
	}

	/*!
	 * \todo
	 * UNSUBSCRIBE cluster & sub-cluster for a client.
	 */
	ret = client_unsubscribe_group(client, cluster, category);
	if (ret == 0) {
		package_alter_instances_to_client(client, ALTER_DESTROY);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_subscribed_category(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *category;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "s", &category);
	if (ret != 1) {
		ErrPrint("Invalid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("[%d] category[%s]\n", pid, category);
	if (!strlen(category)) {
		ErrPrint("Invalid category name: %s\n", category);
		goto out;
	}

	/*!
	 * \TODO
	 * 1. Get a list of created widget instances which are categorized by given "category"
	 * 2. Send created events to the client.
	 * 3. Add this client to "client_only_view_list"
	 */
	if (client_subscribe_category(client, category) == WIDGET_ERROR_NONE) {
		package_alter_instances_to_client(client, ALTER_CREATE);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_unsubscribed_category(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *category;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "s", &category);
	if (ret != 1) {
		ErrPrint("Invalid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	DbgPrint("[%d] category[%s]\n", pid, category);
	if (!strlen(category)) {
		ErrPrint("Invalid category name: %s\n", category);
		goto out;
	}

	/*!
	 * \TODO
	 * 0. Is this client subscribed to given "category"?
	 * 1. Get a list of created widget instances
	 * 2. and then send destroyed event to this client.
	 * 3. Remove this client from the "client_only_view_list"
	 */
	if (client_unsubscribe_category(client, category) == WIDGET_ERROR_NONE) {
		package_alter_instances_to_client(client, ALTER_DESTROY);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static inline __attribute__((always_inline)) struct slave_node *debug_mode_enabled(struct slave_node *slave, int pid, const char *slavename, const char *pkgname, int secured, const char *abi, const char *acceleration)
{
	if (!slave) {
		if (pkgname) {
			slave = slave_find_by_pkgname(pkgname);
		}

		if (!slave) {
			slave = slave_find_by_pid(pid);
			if (!slave) {
				slave = slave_create(slavename, secured, abi, pkgname, 0, acceleration);
				if (!slave) {
					return NULL;
				}
			}

			DbgPrint("New slave is created net(%d) abi(%s) secured(%d) accel(%s)\n", 0, abi, secured, acceleration);
		} else {
			DbgPrint("Registered slave is replaced with this new one\n");
		}
	}

	slave_set_pid(slave, pid);
	slave_set_valid(slave);

	DbgPrint("Provider is forcely activated, pkgname(%s), abi(%s), slavename(%s)\n", pkgname, abi, slavename);
	return slave;
}

static struct packet *slave_hello(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	char _pkgname[pathconf("/", _PC_PATH_MAX)];
	const char *pkgname = NULL;
	struct slave_node *slave;
	const char *acceleration;
	const char *slavename;
	const char *abi;
	int secured;
	int ret;

	ret = packet_get(packet, "isss", &secured, &slavename, &acceleration, &abi);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	if (acceleration[0] == '\0') {
		acceleration = NULL;
	}

	DbgPrint("New slave[%s](%d) is arrived\n", slavename, pid);

	slave = slave_find_by_name(slavename);
	if (!slave) { /* Try again to find a slave using pid */
		slave = slave_find_by_pid(pid);
	}

	if (aul_app_get_pkgname_bypid(pid, _pkgname, sizeof(_pkgname)) != AUL_R_OK) {
		if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
			DbgPrint("Debug mode is enabled. could not determine the pkgname for %d\n", pid);
		} else {
			ErrPrint("pid[%d] is not authroized provider package, try to find it using its name[%s]\n", pid, slavename);
			goto out;
		}
	} else {
		pkgname = (const char *)_pkgname;
	}

	if (!slave) {
		if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
			/**
			 * @note
			 * In case of debugging mode,
			 * Slave can be launched first even there are no instances.
			 * If there is no slave object, the debug_mode_enabled function will create a slave object first.
			 * So it can waiting service request of master.
			 *
			 * But if the slave is launched via valgrind or gdb,
			 * the slave object cannot be created because the aul_app_get_pkgname_bypid will fails to get pkgname of given process.
			 * "gdb" or "valgrind" pid is not exists in the package DB.
			 */
			slave = debug_mode_enabled(slave, pid, slavename, pkgname, secured, abi, acceleration);
			if (!slave) {
				ErrPrint("Failed to create a new slave for %s\n", slavename);
				goto out;
			}
		} else {
			/**
			 * @note
			 * If the master is not in the debug mode,
			 * terminate slave process if it sends connection request which is not authorized.
			 */
			ErrPrint("Request comes from unknown: %d\n", pid);
			ret = aul_terminate_pid_async(pid);
			DbgPrint("Terminate %d (%d)\n", pid, ret);
			goto out;
		}
	} else {
		if (slave_pid(slave) != pid) {
			if (slave_pid(slave) > 0 && !slave_extra_bundle_data(slave) && !(WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode)) {
				ErrPrint("Slave(%s) is already assigned to %d\n", slave_name(slave), slave_pid(slave));
				if (pid > 0) {
					ret = aul_terminate_pid_async(pid);
					DbgPrint("Terminate %d (ret: %d)\n", pid, ret);
				}
				goto out;
			}
			DbgPrint("PID of slave(%s) is updated (%d -> %d) (launched by SDK maybe(%s))\n", slave_name(slave), slave_pid(slave), pid, slave_extra_bundle_data(slave));
			slave_set_pid(slave, pid);
		}

		if (!slave_valid(slave)) {
			char *widget_id;

			widget_id = is_valid_slave(pid, abi, pkgname);
			if (widget_id) {
				slave_set_valid(slave);
				DbgFree(widget_id);
			} else {
				if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
					DbgPrint("Forcely set to valid slave for debugging mode\n");
					slave_set_valid(slave);
				} else {
					ErrPrint("slave is not valid (%s)\n", pkgname);
					goto out;
				}
			}
		}
	}

	/*!
	 * \note
	 * After updating handle,
	 * slave activated callback will be called.
	 */
	slave_rpc_update_handle(slave, handle, 0);

out:
	return NULL;
}

static struct packet *slave_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	int ctrl;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "i", &ctrl);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
	} else {
		slave_set_control_option(slave, ctrl);
	}

out:
	return NULL;
}

static struct packet *slave_ping(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	const char *slavename;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "s", &slavename);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
	} else {
		(void)slave_rpc_ping(slave);
	}

out:
	return NULL;
}

static struct packet *slave_faulted(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &func);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = fault_info_set(slave, pkgname, id, func);
	DbgPrint("Slave Faulted: %s (%d)\n", slave_name(slave), ret);

	pkg = package_find(pkgname);
	if (!pkg) {
		ErrPrint("There is no package info found: %s\n", pkgname);
	} else {
		package_faulted(pkg, 0);
	}

out:
	return NULL;
}

static struct packet *slave_widget_update_begin(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	double priority;
	const char *content;
	const char *title;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssdss", &pkgname, &id, &priority, &content, &title);
	if (ret != 5) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = instance_widget_update_begin(inst, priority, content, title);
		if (ret == (int)WIDGET_ERROR_NONE) {
			slave_freeze_ttl(slave);
		}
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_widget_update_end(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		ret = instance_widget_update_end(inst);
		if (ret == (int)WIDGET_ERROR_NONE) {
			slave_thaw_ttl(slave);
		}
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_gbar_update_begin(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	const struct pkg_info *pkg;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		(void)instance_gbar_update_begin(inst);
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_key_status(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int status;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &status);
	if (ret != 3) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		} else {
			(void)instance_forward_packet(inst, packet_ref((struct packet *)packet));
		}
	}

out:
	return NULL;
}

static struct packet *slave_access_status(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int status;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &status);
	if (ret != 3) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		} else {
			(void)instance_forward_packet(inst, packet_ref((struct packet *)packet));
		}
	}

out:
	return NULL;
}

static struct packet *slave_close_gbar(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int status;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &status);
	if (ret != 3) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		} else {
			(void)instance_forward_packet(inst, packet_ref((struct packet *)packet));
		}
	}

out:
	return NULL;
}

static struct packet *slave_gbar_update_end(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	const struct pkg_info *pkg;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		(void)instance_gbar_update_end(inst);
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_call(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &func);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = fault_func_call(slave, pkgname, id, func);
	slave_give_more_ttl(slave);

out:
	return NULL;
}

static struct packet *slave_ret(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &func);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = fault_func_ret(slave, pkgname, id, func);
	slave_give_more_ttl(slave);

out:
	return NULL;
}

static struct packet *slave_extra_info(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *content_info;
	const char *title;
	const char *icon;
	const char *name;
	double priority;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssssssd", &pkgname, &id, &content_info, &title, &icon, &name, &priority);
	if (ret != 7) {
		ErrPrint("Parameter is not matchd\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
			goto out;
		}

		instance_set_widget_info(inst, priority, content_info, title);
		instance_set_alt_info(inst, icon, name);
		instance_extra_info_updated_by_instance(inst);
		slave_give_more_ttl(slave);
	}

out:
	return NULL;
}

static struct packet *slave_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, width, height, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *safe_filename;
	const char *id;
	int w;
	int h;
	int x;
	int y;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sssiiii", &pkgname, &id, &safe_filename, &x, &y, &w, &h);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
			goto out;
		}

		switch (package_widget_type(instance_package(inst))) {
		case WIDGET_TYPE_SCRIPT:
			script_handler_resize(instance_widget_script(inst), w, h);
			if (safe_filename) {
				(void)script_handler_parse_desc(inst, safe_filename, 0);
			} else {
				safe_filename = widget_util_uri_to_path(id);
				(void)script_handler_parse_desc(inst, safe_filename, 0);
			}

			if (unlink(safe_filename) < 0) {
				ErrPrint("unlink: %d - %s\n", errno, safe_filename);
			}
			break;
		case WIDGET_TYPE_BUFFER:
		default:
			/*!
			 * \check
			 * text format (inst)
			 */
			instance_widget_updated_by_instance(inst, safe_filename, x, y, w, h);
			break;
		}

		/**
		 * @todo
		 * If the slave has direct connection with viewer,
		 * How could master detects its update and how could it be expanded?
		 */
		slave_give_more_ttl(slave);
	}

out:
	return NULL;
}

static struct packet *slave_hold_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int seize;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &seize);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		} else {
			(void)instance_hold_scroll(inst, seize);
		}
	}

out:
	return NULL;
}

static struct packet *slave_extra_updated(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	int idx;
	int x;
	int y;
	int w;
	int h;
	int ret;
	int is_gbar;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssiiiiii", &pkgname, &id, &is_gbar, &idx, &x, &y, &w, &h);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	(void)instance_extra_updated_by_instance(inst, is_gbar, idx, x, y, w, h);
out:
	return NULL;
}

static struct packet *slave_desc_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, decsfile, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *descfile;
	int x;
	int y;
	int w;
	int h;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sssiiii", &pkgname, &id, &descfile, &x, &y, &w, &h);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	switch (package_gbar_type(instance_package(inst))) {
	case GBAR_TYPE_SCRIPT:
		DbgPrint("%s updated (%s)\n", instance_id(inst), descfile);
		if (script_handler_is_loaded(instance_gbar_script(inst))) {
			(void)script_handler_parse_desc(inst, descfile, 1);
		}
		break;
	case GBAR_TYPE_TEXT:
		instance_set_gbar_size(inst, 0, 0);
	case GBAR_TYPE_BUFFER:
		instance_gbar_updated(pkgname, id, descfile, x, y, w, h);
		break;
	default:
		DbgPrint("Ignore updated DESC(%s)\n", pkgname);
		break;
	}

out:
	return NULL;
}

static struct packet *slave_deleted(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, id, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, NULL);
	if (ret == (int)WIDGET_ERROR_NONE) {
		ret = instance_destroyed(inst, WIDGET_ERROR_NONE);
	}

out:
	return NULL;
}

/*!
 * \note for the BUFFER Type slave
 */
static struct packet *slave_acquire_buffer(pid_t pid, int handle, const struct packet *packet) /* type, id, w, h, size */
{
	enum target_type target;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	int pixel_size;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		id = "";
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "issiii", &target, &pkgname, &id, &w, &h, &pixel_size);
	if (ret != 6) {
		ErrPrint("Invalid argument\n");
		id = "";
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	id = "";

	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	ret = WIDGET_ERROR_INVALID_PARAMETER;

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", package_name(pkg));
		goto out;
	}

	if (target == TYPE_WIDGET && package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_widget_buffer(inst);
		if (!info) {
			if (!instance_create_widget_buffer(inst, pixel_size)) {
				ErrPrint("Failed to create a WIDGET buffer\n");
				ret = WIDGET_ERROR_FAULT;
				goto out;
			}

			info = instance_widget_buffer(inst);
			if (!info) {
				ErrPrint("WIDGET buffer is not valid\n");
				/*!
				 * \NOTE
				 * ret value should not be changed.
				 */
				goto out;
			}
		}

		ret = buffer_handler_resize(info, w, h, pixel_size);
		ret = buffer_handler_load(info);
		if (ret == 0) {
			instance_set_widget_size(inst, w, h);
			instance_set_widget_info(inst, WIDGET_CONF_PRIORITY_NO_CHANGE, WIDGET_CONF_CONTENT_NO_CHANGE, WIDGET_CONF_TITLE_NO_CHANGE);
			id = buffer_handler_id(info);
		} else {
			ErrPrint("Failed to load a buffer(%d)\n", ret);
		}
	} else if (target == TYPE_GBAR && package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		struct buffer_info *info;
		Ecore_Timer *gbar_monitor;
		int is_resize;

		is_resize = 0;
		gbar_monitor = instance_del_data(inst, GBAR_OPEN_MONITOR_TAG);
		if (!gbar_monitor) {
			gbar_monitor = instance_del_data(inst, GBAR_RESIZE_MONITOR_TAG);
			is_resize = !!gbar_monitor;
			if (!is_resize) {
				/* Invalid request. Reject this */
				ErrPrint("Invalid request\n");
				goto out;
			}

			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst);
		} else {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst);
		}

		ecore_timer_del(gbar_monitor);
		inst = instance_unref(inst);
		if (!inst) {
			ErrPrint("Instance refcnt is ZERO: %s\n", package_name(pkg));
			goto out;
		}

		info = instance_gbar_buffer(inst);
		if (!info) {
			if (!instance_create_gbar_buffer(inst, pixel_size)) {
				ErrPrint("Failed to create a GBAR buffer\n");
				ret = WIDGET_ERROR_FAULT;
				instance_client_gbar_created(inst, ret);
				goto out;
			}

			info = instance_gbar_buffer(inst);
			if (!info) {
				ErrPrint("GBAR buffer is not valid\n");
				/*!
				 * \NOTE
				 * ret value should not be changed.
				 */
				instance_client_gbar_created(inst, ret);
				goto out;
			}
		}

		ret = buffer_handler_resize(info, w, h, pixel_size);
		ret = buffer_handler_load(info);
		if (ret == 0) {
			instance_set_gbar_size(inst, w, h);
			id = buffer_handler_id(info);
		} else {
			ErrPrint("Failed to load a buffer (%d)\n", ret);
		}

		/*!
		 * Send the GBAR created event to the client
		 */
		if (!is_resize) {
			instance_client_gbar_created(inst, ret);
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *slave_acquire_extra_buffer(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	struct packet *result;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	int pixel_size;
	int target;
	int idx;
	int ret;
	int w;
	int h;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		id = "";
		goto out;
	}

	ret = packet_get(packet, "issiiii", &target, &pkgname, &id, &w, &h, &pixel_size, &idx);
	if (ret != 7) {
		ErrPrint("Invalid parameters\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		id = "";
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	id = "";

	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	ret = WIDGET_ERROR_INVALID_PARAMETER;

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (target == TYPE_WIDGET && package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_widget_extra_buffer(inst, idx);
		if (!info) {
			if (!instance_create_widget_extra_buffer(inst, pixel_size, idx)) {
				ErrPrint("Failed to create a WIDGET buffer\n");
				ret = WIDGET_ERROR_FAULT;
				goto out;
			}

			info = instance_widget_extra_buffer(inst, idx);
			if (!info) {
				ErrPrint("WIDGET extra buffer is not valid\n");
				/*!
				 * \NOTE
				 * ret value should not be changed.
				 */
				goto out;
			}
		}

		ret = buffer_handler_resize(info, w, h, pixel_size);
		ret = buffer_handler_load(info);
		if (ret == 0) {
			/**
			 * @todo
			 * Send the extra buffer info to the viewer.
			 * Then the viewer will try to acquire extra pixmap(aka, resource id) information
			 */
			id = buffer_handler_id(info);
			DbgPrint("Extra buffer is loaded: %s\n", id);
			(void)instance_client_widget_extra_buffer_created(inst, idx);
		} else {
			ErrPrint("Failed to load a buffer(%d)\n", ret);
		}
	} else if (target == TYPE_GBAR && package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_gbar_extra_buffer(inst, idx);
		if (!info) {
			if (!instance_create_gbar_extra_buffer(inst, pixel_size, idx)) {
				ErrPrint("Failed to create a GBAR buffer\n");
				ret = WIDGET_ERROR_FAULT;
				goto out;
			}

			info = instance_gbar_extra_buffer(inst, idx);
			if (!info) {
				ErrPrint("GBAR buffer is not valid\n");
				/*!
				 * \NOTE
				 * ret value should not be changed.
				 */
				goto out;
			}
		}

		ret = buffer_handler_resize(info, w, h, pixel_size);
		ret = buffer_handler_load(info);
		if (ret == 0) {
			id = buffer_handler_id(info);
			/**
			 * @todo
			 * Send the extra buffer acquired event to the viewer
			 */
			DbgPrint("Extra buffer is loaded: %s\n", id);
			(void)instance_client_gbar_extra_buffer_created(inst, idx);
		} else {
			ErrPrint("Failed to load a buffer (%d)\n", ret);
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *slave_resize_buffer(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct packet *result;
	enum target_type type;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	struct inst_info *inst = NULL;
	const struct pkg_info *pkg = NULL;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		ret = WIDGET_ERROR_NOT_EXIST;
		id = "";
		goto out;
	}

	ret = packet_get(packet, "issii", &type, &pkgname, &id, &w, &h);
	if (ret != 5) {
		ErrPrint("Invalid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		id = "";
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	id = "";

	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	ret = WIDGET_ERROR_INVALID_PARAMETER;
	/*!
	 * \note
	 * Reset "id", It will be re-used from here
	 */

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (type == TYPE_WIDGET && package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_widget_buffer(inst);
		if (!info) {
			goto out;
		}

		ret = buffer_handler_resize(info, w, h, 0);
		/*!
		 * \note
		 * id is resued for newly assigned ID
		 */
		if (ret == (int)WIDGET_ERROR_NONE) {
			id = buffer_handler_id(info);
			instance_set_widget_size(inst, w, h);
			instance_set_widget_info(inst, WIDGET_CONF_PRIORITY_NO_CHANGE, WIDGET_CONF_CONTENT_NO_CHANGE, WIDGET_CONF_TITLE_NO_CHANGE);
		}
	} else if (type == TYPE_GBAR && package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_gbar_buffer(inst);
		if (!info) {
			goto out;
		}

		ret = buffer_handler_resize(info, w, h, 0);
		/*!
		 * \note
		 * id is resued for newly assigned ID
		 */
		if (ret == (int)WIDGET_ERROR_NONE) {
			id = buffer_handler_id(info);
			instance_set_gbar_size(inst, w, h);
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *slave_release_buffer(pid_t pid, int handle, const struct packet *packet)
{
	enum target_type type;
	const char *pkgname;
	const char *id;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	if (packet_get(packet, "iss", &type, &pkgname, &id) != 3) {
		ErrPrint("Inavlid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	ret = WIDGET_ERROR_INVALID_PARAMETER;

	if (type == TYPE_WIDGET && package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_widget_buffer(inst);
		ret = buffer_handler_unload(info);
	} else if (type == TYPE_GBAR && package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		struct buffer_info *info;
		Ecore_Timer *gbar_monitor;

		gbar_monitor = instance_del_data(inst, GBAR_CLOSE_MONITOR_TAG);
		if (!gbar_monitor && !package_is_fault(pkg)) {
			ErrPrint("Slave requests to release a buffer\n");
			/**
			 * @note
			 * In this case just keep going to release buffer,
			 * Even if a user(client) doesn't wants to destroy the GBAR.
			 *
			 * If the slave tries to destroy GBAR buffer, it should be
			 * released and reported to the client about its status.
			 *
			 * Even if the pd is destroyed by timeout handler,
			 * instance_client_gbar_destroyed function will be ignored
			 * by pd.need_to_send_close_event flag.
			 * which will be checked by instance_client_gbar_destroyed function.
			 */

			/**
			 * @note
			 * provider can try to resize the buffer size.
			 * in that case, it will release the buffer first.
			 * Then even though the client doesn't request to close the GBAR,
			 * the provider can release it.
			 * If we send the close event to the client,
			 * The client will not able to allocate GBAR again.
			 * In this case, add the pd,monitor again. from here.
			 * to wait the re-allocate buffer.
			 * If the client doesn't request buffer reallocation,
			 * Treat it as a fault. and close the GBAR.
			 */
			info = instance_gbar_buffer(inst);
			ret = buffer_handler_unload(info);

			if (ret == (int)WIDGET_ERROR_NONE) {
				gbar_monitor = ecore_timer_add(WIDGET_CONF_GBAR_REQUEST_TIMEOUT, gbar_resize_monitor_cb, instance_ref(inst));
				if (!gbar_monitor) {
					ErrPrint("Failed to create a timer for GBAR Open monitor\n");
					inst = instance_unref(inst);
					if (!inst) {
						DbgPrint("Instance is deleted\n");
					}
				} else {
					(void)instance_set_data(inst, GBAR_RESIZE_MONITOR_TAG, gbar_monitor);
					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst) != WIDGET_ERROR_NONE) {
						ErrPrint("Failed to add event handler: %s\n", pkgname);
					}
				}
			}
		} else {
			if (gbar_monitor) {
				/**
				 * @note
				 * If the instance has gbar_monitor, the pd close requested from client via client_destroy_gbar.
				 */
				slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
				ecore_timer_del(gbar_monitor);

				inst = instance_unref(inst);
				if (!inst) {
					ErrPrint("Instance is released: %s\n", pkgname);
					ret = WIDGET_ERROR_FAULT;
					goto out;
				}
			} /* else {
				 @note
				 This case means that the package is faulted so the service provider tries to release the buffer
			   */

			info = instance_gbar_buffer(inst);
			ret = buffer_handler_unload(info);

			/**
			 * @note
			 * Send the GBAR destroyed event to the client
			 */
			instance_client_gbar_destroyed(inst, ret);
		}
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *slave_release_extra_buffer(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct packet *result;
	const char *id;
	struct buffer_info *info = NULL;
	int ret;
	int idx;
	int type;
	const char *pkgname;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = WIDGET_ERROR_NOT_EXIST;
		goto out;
	}

	if (packet_get(packet, "issi", &type, &pkgname, &id, &idx) != 4) {
		ErrPrint("Inavlid argument\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = validate_request(pid, slave, pkgname, id, &inst, &pkg);
	if (ret != WIDGET_ERROR_NONE) {
		goto out;
	}

	ret = WIDGET_ERROR_INVALID_PARAMETER;

	if (type == TYPE_WIDGET && package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
		info = instance_widget_extra_buffer(inst, idx);
		(void)instance_client_widget_extra_buffer_destroyed(inst, idx);
	} else if (type == TYPE_GBAR && package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
		info = instance_gbar_extra_buffer(inst, idx);
		(void)instance_client_gbar_extra_buffer_destroyed(inst, idx);
	}

	if (info) {
		ret = buffer_handler_unload(info);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static void delete_ctx_cb(int handle, void *data)
{
	struct sync_ctx_item *ctx = data;

	s_info.hello_sync_ctx_list = eina_list_remove(s_info.hello_sync_ctx_list, ctx);

	if (ctx->handle != handle) {
		ErrPrint("Context is not valid (%s)\n", ctx->pkgname);
	}

	DbgFree(ctx->pkgname);
	DbgFree(ctx);
}

static struct packet *slave_hello_sync_prepare(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	char _pkgname[pathconf("/", _PC_PATH_MAX)];
	const char *pkgname = NULL;
	int ret;
	struct sync_ctx_item *ctx;
	double timestamp;
	Eina_List *l;
	Eina_List *n;

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	if (aul_app_get_pkgname_bypid(pid, _pkgname, sizeof(_pkgname)) != AUL_R_OK) {
		if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
			DbgPrint("Failed to get pkgname of %d\n", pid);
		} else {
			ErrPrint("pid[%d] is not authroized provider package, try to find it using its name\n", pid);
			goto out;
		}
	} else {
		pkgname = (const char *)_pkgname;
	}

	ctx = NULL;
	EINA_LIST_FOREACH_SAFE(s_info.hello_sync_ctx_list, l, n, ctx) {
		if (ctx->timestamp == timestamp) {
			if (pkgname && ctx->pkgname) {
				if (strcmp(pkgname, ctx->pkgname)) {
					ErrPrint("timestamp is valid, but pkgname is not matched: %s <> %s\n", pkgname, ctx->pkgname);
					/* Go ahead */
				}
			} else {
				DbgPrint("Skip to pkgname comparison (%s <> %s)\n", pkgname, ctx->pkgname);
			}

			break;
		}
		ctx = NULL;
	}

	if (ctx) {
		struct slave_node *slave;
		/**
		 * @note
		 * This is only possible that the hello_sync comes first.
		 * In that case, we should activate the slave from here.
		 */
		s_info.hello_sync_ctx_list  = eina_list_remove(s_info.hello_sync_ctx_list, ctx);
		if (ctx->prepare_sync_wait_timer) {
			ecore_timer_del(ctx->prepare_sync_wait_timer);
		}
		DbgFree(ctx->pkgname);
		DbgFree(ctx);

		slave = slave_find_by_pid(pid);
		if (!slave) {
			ErrPrint("Unable to activate slave %d\n", pid);
			goto out;
		}

		DbgPrint("Update handle for %s (%d)\n", pkgname, handle);
		slave_rpc_update_handle(slave, handle, 1);
	} else {
		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			ErrPrint("calloc: %d\n", errno);
			goto out;
		}

		ctx->timestamp = timestamp;

		if (pkgname) {
			ctx->pkgname = strdup(pkgname);
			if (!ctx->pkgname) {
				ErrPrint("strdup: %d\n", errno);
				DbgFree(ctx);
				goto out;
			}
		}

		ctx->pid = pid;
		ctx->handle = handle;

		DbgPrint("Sync context created: %s (%d)\n", pkgname, pid);
		s_info.hello_sync_ctx_list = eina_list_append(s_info.hello_sync_ctx_list, ctx);

		if (dead_callback_add(handle, delete_ctx_cb, ctx) < 0) {
			ErrPrint("Failed to add dead callback\n");
		}
	}

out:
	return NULL;
}

static Eina_Bool prepare_sync_wait_cb(void *data)
{
	struct sync_ctx_item *item = data;

	s_info.hello_sync_ctx_list = eina_list_remove(s_info.hello_sync_ctx_list, item);

	ErrPrint("Sync timer(%lf) expired (%s), Terminate %d\n", WIDGET_CONF_SLAVE_ACTIVATE_TIME * 2.0f, item->pkgname, item->pid);

	if (item->pid > 0) {
		aul_terminate_pid_async(item->pid);
	}

	item->prepare_sync_wait_timer = NULL;
	DbgFree(item->pkgname);
	DbgFree(item);

	return ECORE_CALLBACK_CANCEL;
}

static struct packet *slave_hello_sync(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	struct packet *result = NULL;
	struct slave_node *slave;
	const char *slavename;
	const char *slave_pkgname;
	const char *acceleration;
	const char *abi;
	char *widget_id;
	int secured;
	int ret;
	char _pkgname[pathconf("/", _PC_PATH_MAX)];
	double timestamp;
	const char *pkgname = NULL;

	ret = packet_get(packet, "disssss", &timestamp, &secured, &slavename, &slave_pkgname, &acceleration, &abi, &widget_id);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (acceleration[0] == '\0') {
		acceleration = NULL;
	}

	DbgPrint("New slave[%s](%d) is arrived.\n", slavename, pid);

	/**
	 * In case of watch-app, the slavename is declared by app itself.
	 * So slave_find_by_name will always fail.
	 * But the slave object can be found from slave_find_by_pid.
	 */
	slave = slave_find_by_name(slavename);
	if (!slave) { /* Try again to find a slave using pid */
		slave = slave_find_by_pid(pid);
		if (!slave) {
			slave = slave_find_by_pkgname(slave_pkgname);
			ErrPrint("Failed to find, Find it using pkgname(%s) - %p\n", slave_pkgname, slave);
		}
	}

	if (aul_app_get_pkgname_bypid(pid, _pkgname, sizeof(_pkgname)) != AUL_R_OK) {
		if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
			pkgname = slave_pkgname;
			DbgPrint("Debug mode is enabled. could not determine the pkgname for %d use the slave_pkgname[%s]\n", pid, pkgname);
		} else {
			ErrPrint("pid[%d] is not authroized provider package, try to find it using its name[%s]\n", pid, slavename);
			ret = WIDGET_ERROR_PERMISSION_DENIED;
			goto out;
		}
	} else {
		pkgname = (const char *)_pkgname;
	}

	if (slave) {
		struct sync_ctx_item *item;
		Eina_List *l;
		Eina_List *n;

		item = NULL;
		EINA_LIST_FOREACH_SAFE(s_info.hello_sync_ctx_list, l, n, item) {
			if (item->timestamp == timestamp && item->pid == pid) {
				s_info.hello_sync_ctx_list = eina_list_remove(s_info.hello_sync_ctx_list, item);
				break;
			}
			item = NULL;
		}

		if (!item) {
			ErrPrint("There is no such sync context (%d), Waiting prepare ctx\n", pid);

			item = calloc(1, sizeof(*item));
			if (!item) {
				ErrPrint("calloc: %d\n", errno);
				ret = WIDGET_ERROR_OUT_OF_MEMORY;
				goto out;
			}

			item->pkgname = strdup(pkgname);
			if (!item->pkgname) {
				ErrPrint("strdup: %d\n", errno);
				DbgFree(item);
				ret = WIDGET_ERROR_OUT_OF_MEMORY;
				goto out;
			}

			item->handle = -1;
			item->pid = pid;
			item->timestamp = timestamp;

			if (!(WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode)) {
				/**
				 * @note
				 * If the prepare sync doesn't come in ACTIVATE_TIME * 2.0f, there is a problem. we have to cancel it
				 */
				item->prepare_sync_wait_timer = ecore_timer_add(WIDGET_CONF_SLAVE_ACTIVATE_TIME * 2.0f, prepare_sync_wait_cb, item);
			} else {
				DbgPrint("prepare sync wait timer is disabled for debugging\n");
			}

			/**
			 * @note
			 * How can we delete this if there is no prepare context?
			 */
			s_info.hello_sync_ctx_list = eina_list_append(s_info.hello_sync_ctx_list, item);

			/**
			 * @note
			 * Reset the handle to prevent from updating RPC handle of SLAVE object
			 */
			handle = -1;
		} else {
			if (item->pkgname) {
				if (strcmp(item->pkgname, pkgname)) {
					ErrPrint("HELLO_SYNC is comes from different package: %s <> %s\n", item->pkgname, pkgname);
				}
			} else {
				DbgPrint("item->pkgname[%s]\n", item->pkgname);
			}

			handle = item->handle;

			if (dead_callback_del(handle, delete_ctx_cb, item) != item) {
				ErrPrint("Dead callback is not valid\n");
			}

			DbgFree(item->pkgname);
			DbgFree(item);

			DbgPrint("Hello sync context found: %s\n", pkgname);
		}
	}

	if (!slave) {
		if (WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode) {
			slave = debug_mode_enabled(slave, pid, slavename, pkgname, secured, abi, acceleration);
			if (!slave) {
				ErrPrint("Failed to create a new slave for %s\n", slavename);
				ret = WIDGET_ERROR_FAULT;
				goto out;
			}
		} else {
			ErrPrint("There is no valid slave instance. ignore this %s\n", slavename);
			ret = WIDGET_ERROR_NOT_EXIST;
			goto out;
		}
	} else {
		struct pkg_info *info;
		struct inst_info *inst;
		int width, height;
		unsigned int widget_size;
		Eina_List *inst_list;
		char *tmp_widget_id;

		tmp_widget_id = is_valid_slave(pid, abi, pkgname);
		if (!tmp_widget_id) {
			goto out;
		}

		if (strcmp(widget_id, tmp_widget_id)) {
			DbgPrint("[%s] <> [%s]\n", widget_id, tmp_widget_id);
		}

		DbgFree(tmp_widget_id);

		if (!slave_is_watch(slave)) {
			ErrPrint("Slave is not watch(only watch can use hello_sync) [%s]\n", widget_id);
			goto out;
		}

		info = package_find(widget_id);
		if (!info) {
			char *pkgid;

			pkgid = widget_service_get_package_id(widget_id);
			if (!pkgid) {
				goto out;
			}

			DbgPrint("Package information is not exists, create it for [%s]\n", widget_id);
			info = package_create(pkgid, widget_id);
			DbgFree(pkgid);

			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		inst_list = package_instance_list(info);
		inst = eina_list_data_get(eina_list_last(inst_list));
		if (!inst) {
			ErrPrint("Instance is not available for [%s]\n", widget_id);
			ret = WIDGET_ERROR_FAULT;
			goto out;
		}

		DbgPrint("[%s] Instance Count: %d\n", widget_id, eina_list_count(inst_list));

		if (slave_pid(slave) != pid) {
			if (slave_pid(slave) > 0 && !slave_extra_bundle_data(slave) && !(WIDGET_CONF_DEBUG_MODE || g_conf.debug_mode)) {
				CRITICAL_LOG("Slave(%s) is already assigned to %d\n", slave_name(slave), slave_pid(slave));
				if (pid > 0) {
					ret = aul_terminate_pid_async(pid);
					CRITICAL_LOG("Terminate %d (ret: %d)\n", pid, ret);
				}
				ret = WIDGET_ERROR_NOT_EXIST;
				goto out;
			}
			DbgPrint("PID of slave(%s) is updated (%d -> %d)\n", slave_name(slave), slave_pid(slave), pid);
			slave_set_pid(slave, pid);
		}

		slave_set_valid(slave);

		if (handle >= 0) {
			slave_rpc_update_handle(slave, handle, 1);

			/**
			 * @note
			 * This PAUSE/RESUME state will be sent to the provider after the instance is created.
			 * Because, this command is sync. so there is no way to get the state packet first then create packet.
			 */
		} else {
			DbgPrint("Slave RPC should be updated soon (waiting prepare sync)\n");
		}

		DbgPrint("Slave is activated by request: %d (%s)/(%s)\n", pid, pkgname, slavename);
		widget_size = package_size_list(info);

		if (widget_size & WIDGET_SIZE_TYPE_2x2) {
			widget_service_get_size(WIDGET_SIZE_TYPE_2x2, &width, &height);
		} else if (widget_size & WIDGET_SIZE_TYPE_4x4) {
			widget_service_get_size(WIDGET_SIZE_TYPE_4x4, &width, &height);
		} else {
			widget_service_get_size(WIDGET_SIZE_TYPE_1x1, &width, &height);
			DbgPrint("widget(%s] does not support size [2x2], [4x4]\n",pkgname);
		}

		(void)instance_watch_change_package_info(inst, info);
		result = instance_duplicate_packet_create(packet, inst, width, height);

		if (instance_watch_need_to_recover(inst)) {
			/**
			 * @note
			 * Reset the state recover flag.
			 */
			(void)instance_watch_set_need_to_recover(inst, EINA_FALSE);

			/**
			 * @note
			 * Re-send the visible state change request in this case.
			 * But, this will be sent after return from this function.
			 * Request will be sent from command comsumer which are running by timer callback.
			 * (slave_rpc_request_only, slave_rpc_async_request, ...)
			 */
			(void)instance_watch_recover_visible_state(inst);
		}
	}

out:
	if (!result) {
		DbgPrint("## ret [%d]\n", ret);
		result = packet_create_reply(packet, "i", ret);
		if (!result) {
			ErrPrint("Failed to create a reply packet\n");
		}
	}
	return result;
}

static struct packet *service_instance_count(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct pkg_info *pkg;
	double timestamp;
	const char *pkgname;
	const char *cluster;
	const char *category;
	Eina_List *pkg_list;
	Eina_List *l;
	Eina_List *inst_list;
	Eina_List *inst_l;
	struct inst_info *inst;
	int ret;

	ret = packet_get(packet, "sssd", &pkgname, &cluster, &category, &timestamp);
	if (ret != 4) {
		ErrPrint("Invalid packet\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	pkg_list = (Eina_List *)package_list();

	ret = 0;
	EINA_LIST_FOREACH(pkg_list, l, pkg) {
		if (pkgname && pkgname[0]) {
			if (strcmp(package_name(pkg), pkgname)) {
				continue;
			}
		}

		inst_list = package_instance_list(pkg);
		EINA_LIST_FOREACH(inst_list, inst_l, inst) {
			if (cluster && cluster[0]) {
				if (strcmp(instance_cluster(inst), cluster)) {
					continue;
				}
			}

			if (category && category[0]) {
				if (strcmp(instance_category(inst), category)) {
					continue;
				}
			}

			ret++;
		}
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *service_get_content(pid_t pid, int handle, const struct packet *packet)
{
	const char *inst_id;
	const char *widget_id;
	struct inst_info *inst;
	struct packet *result;

	if (packet_get(packet, "ss", &widget_id, &inst_id) != 2) {
		ErrPrint("Invalid parameter\n");
		return NULL;
	}

	if (is_valid_service_requestor(pid, widget_id) == 0) {
		ErrPrint("Invalid service requestor: %d %s\n", pid, widget_id);
		result = packet_create_reply(packet, "is", WIDGET_ERROR_PERMISSION_DENIED, "?");
		return result;
	}

	inst = package_find_instance_by_id(widget_id, inst_id);
	if (!inst) {
		result = packet_create_reply(packet, "is", WIDGET_ERROR_NOT_EXIST, "?");
	} else {
		result = packet_create_reply(packet, "is", WIDGET_ERROR_NONE, instance_content(inst));
	}

	if (!result) {
		ErrPrint("Failed to create a result packet\n");
	}

	return result;
}

static struct packet *service_get_inst_list(pid_t pid, int handle, const struct packet *packet)
{
	const char *widget_id;
	struct pkg_info *pkg;
	struct packet *result;

	if (packet_get(packet, "s", &widget_id) != 1) {
		ErrPrint("Invalid parameter\n");
		return NULL;
	}

	if (is_valid_service_requestor(pid, widget_id) == 0) {
		ErrPrint("Invalid requestor: %d %s\n", pid, widget_id);
		result = packet_create_reply(packet, "iis", WIDGET_ERROR_PERMISSION_DENIED, 0, NULL);
		return result;
	}

	pkg = package_find(widget_id);
	if (!pkg) {
		result = packet_create_reply(packet, "iis", WIDGET_ERROR_NOT_EXIST, 0, NULL);
	} else {
		Eina_List *inst_list;

		inst_list = package_instance_list(pkg);
		if (!inst_list) {
			result = packet_create_reply(packet, "iis", WIDGET_ERROR_NOT_EXIST, 0, NULL);
		} else {
			char *id_buffer;
			int size = sysconf(_SC_PAGESIZE);
			Eina_List *l;
			struct inst_info *inst;
			int offset;
			int len;
			int cnt = 0;

			if (size < 0) {
				ErrPrint("sysconf: %d\n", errno);
				size = PAGE_SIZE;
			}

			id_buffer = malloc(size);
			if (!id_buffer) {
				result = packet_create_reply(packet, "iis", WIDGET_ERROR_OUT_OF_MEMORY, 0, NULL);
				goto out;
			}

			offset = 0;
			EINA_LIST_FOREACH(inst_list, l, inst) {
				len = strlen(instance_id(inst));
				if (offset + len > size) {
					/* Expanding the ID_BUFFER */
					char *resized_buffer;
					int more_size;
					more_size = sysconf(_SC_PAGESIZE);
					if (more_size < 0) {
						ErrPrint("sysconf: %d\n", errno);
						more_size = PAGE_SIZE;
					}
					size += more_size;

					DbgPrint("Expanding heap to %d\n", size);

					resized_buffer = realloc(id_buffer, size);
					if (!resized_buffer) {
						ErrPrint("realloc: %d\n", errno);
						DbgFree(id_buffer);
						result = packet_create_reply(packet, "iis", WIDGET_ERROR_OUT_OF_MEMORY, 0, NULL);
						goto out;
					}

					id_buffer = resized_buffer;
					cnt++;
				}

				strcpy(id_buffer + offset, instance_id(inst));
				/* Replace last NULL with NEW_LINE */
				id_buffer[offset + len] = '\n';
				offset += (len + 1);
			}

			id_buffer[offset] = '\0';
			result = packet_create_reply(packet, "iis", WIDGET_ERROR_NONE, cnt, id_buffer);
			DbgFree(id_buffer);
		}
	}

out:
	return result;
}

static struct packet *monitor_register(pid_t pid, int handle, const struct packet *packet)
{
	const char *widget_id;

	if (packet_get(packet, "s", &widget_id) != 1) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (widget_id[0] == '*' && widget_id[1] == '\0') {
		widget_id = NULL;
	}

	DbgPrint("Register monitor target for [%s]\n", widget_id);
	if (!widget_id) {
		/**
		 * @TODO
		 *  Only the authorized requstor can listens all widget's lifecycle event.
		WIDGET_CONF_MONITOR_PACKAGE required
		 */
		if (widget_mgr_is_valid_requestor(pid) == 0) {
			ErrPrint("All widget's lifecycle event listener is not registered\n");
			goto out;
		}
	} else {
		if (is_valid_service_requestor(pid, widget_id) == 0) {
			goto out;
		}
	}

	if (monitor_find_client_by_pid(widget_id, pid)) {
		ErrPrint("Already registered: [%s], %d\n", widget_id, pid);
	} else {
		if (monitor_create_client(widget_id, pid, handle) == NULL) {
			ErrPrint("Failed to create a new monitor client\n");
		}
	}

out:
	return NULL;
}

static struct packet *monitor_unregister(pid_t pid, int handle, const struct packet *packet)
{
	const char *widget_id;
	struct monitor_client *monitor;

	if (packet_get(packet, "s", &widget_id) != 1) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (widget_id[0] == '*' && widget_id[1] == '\0') {
		widget_id = NULL;
	}

	monitor = monitor_find_client_by_pid(widget_id, pid);
	if (monitor) {
		DbgPrint("Unregister monitor target: %s\n", widget_id);
		monitor_destroy_client(monitor);
	} else {
		ErrPrint("Monitor for %s is not exists(%d)\n", widget_id, pid);
	}

out:
	return NULL;
}

static struct packet *service_change_period(pid_t pid, int handle, const struct packet *packet)
{
	struct inst_info *inst;
	struct packet *result;
	const char *pkgname;
	const char *id;
	double period;
	int ret;

	ret = packet_get(packet, "ssd", &pkgname, &id, &period);
	if (ret != 3) {
		ErrPrint("Invalid packet\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (is_valid_service_requestor(pid, pkgname) == 0) {
		ErrPrint("Invalid requestor: %d (%s)\n", pid, pkgname);
		ret = WIDGET_ERROR_PERMISSION_DENIED;
		goto out;
	}

	if (!strlen(id)) {
		struct pkg_info *pkg;

		pkg = package_find(pkgname);
		if (!pkg) {
			ret = WIDGET_ERROR_NOT_EXIST;
		} else if (package_is_fault(pkg)) {
			ret = WIDGET_ERROR_FAULT;
		} else {
			Eina_List *inst_list;
			Eina_List *l;

			inst_list = package_instance_list(pkg);
			EINA_LIST_FOREACH(inst_list, l, inst) {
				ret = instance_set_period(inst, period);
				if (ret < 0) {
					ErrPrint("Failed to change the period of %s to (%lf)\n", pkgname, period);
				}
			}
		}
	} else {
		ret = validate_request(pid, NULL, pkgname, id, &inst, NULL);
		if (ret == (int)WIDGET_ERROR_NONE) {
			if (instance_state(inst) == INST_DESTROYED) {
				ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
				ret = WIDGET_ERROR_INVALID_PARAMETER;
				goto out;
			}

			ret = instance_set_period(inst, period);
		}
	}

	DbgPrint("Change the update period: %s, %lf : %d\n", pkgname, period, ret);
out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *service_update(pid_t pid, int handle, const struct packet *packet)
{
	Eina_List *inst_list;
	struct pkg_info *pkg;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *cluster;
	const char *category;
	const char *content;
	int force;
	char *widget_id;
	int ret;

	ret = packet_get(packet, "sssssi", &pkgname, &id, &cluster, &category, &content, &force);
	if (ret != 6) {
		ErrPrint("Invalid Packet\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	if (is_valid_service_requestor(pid, pkgname) == 0) {
		ErrPrint("Requestor is not valid\n");
		ret = WIDGET_ERROR_PERMISSION_DENIED;
		goto out;
	}

	widget_id = package_widget_pkgname(pkgname);
	if (!widget_id) {
		ErrPrint("Invalid package %s\n", pkgname);
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	pkg = package_find(widget_id);
	if (!pkg) {
		ret = WIDGET_ERROR_NOT_EXIST;
		DbgFree(widget_id);
		goto out;
	}

	if (package_is_fault(pkg)) {
		ret = WIDGET_ERROR_FAULT;
		DbgFree(widget_id);
		goto out;
	}

	inst_list = package_instance_list(pkg);
	if (!eina_list_count(inst_list)) {
		ret = WIDGET_ERROR_NOT_EXIST;
		DbgFree(widget_id);
		goto out;
	}

	if (id && strlen(id)) {
		Eina_List *l;
		struct inst_info *inst;

		ret = WIDGET_ERROR_NOT_EXIST;
		EINA_LIST_FOREACH(inst_list, l, inst) {
			if (!strcmp(instance_id(inst), id)) {
				ret = WIDGET_ERROR_NONE;
				break;
			}
		}

		if (ret == (int)WIDGET_ERROR_NOT_EXIST) {
			DbgFree(widget_id);
			goto out;
		}
	}

	/*!
	 * \TODO
	 * Validate the update requstor.
	 */
	slave_rpc_request_update(widget_id, id, cluster, category, content, force);
	DbgFree(widget_id);
	ret = WIDGET_ERROR_NONE;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *widget_mgr_hello(pid_t pid, int handle, const struct packet *packet)
{
	struct widget_mgr *info;
	struct packet *result;
	int ret;
	const char *fifo_name;
	double timestamp;

	DbgPrint("Request arrived from %d\n", pid);

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid packet\n");
		fifo_name = "";
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	info = widget_mgr_create(pid, handle);
	if (!info) {
		ErrPrint("Failed to create a widget_mgr object\n");
		fifo_name = "";
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		goto out;
	}

	ret = 0;
	fifo_name = widget_mgr_filename(info);
	DbgPrint("FIFO Created: %s (Serve for %d)\n", fifo_name, pid);

out:
	result = packet_create_reply(packet, "si", fifo_name, ret);
	if (!result) {
		ErrPrint("Failed to create a result packet\n");
	}

	return result;
}

static Eina_Bool lazy_slave_list_cb(void *info)
{
	FILE *fp;
	Eina_List *list;
	Eina_List *l;
	struct slave_node *slave;

	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		widget_mgr_close_fifo(info);
		return ECORE_CALLBACK_CANCEL;
	}

	list = (Eina_List *)slave_list();
	EINA_LIST_FOREACH(list, l, slave) {
		fprintf(fp, "%d %s %s %s %d %d %d %s %d %d %lf\n", 
				slave_pid(slave),
				slave_name(slave),
				slave_pkgname(slave),
				slave_abi(slave),
				slave_is_secured(slave),
				slave_refcnt(slave),
				slave_fault_count(slave),
				slave_state_string(slave),
				slave_loaded_instance(slave),
				slave_loaded_package(slave),
				slave_ttl(slave)
			   );
	}

	fprintf(fp, "EOD\n");
	widget_mgr_close_fifo(info);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *widget_mgr_slave_list(pid_t pid, int handle, const struct packet *packet)
{
	struct widget_mgr *info;
	double timestamp;

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = widget_mgr_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	lazy_slave_list_cb(info);
out:
	return NULL;
}

static inline const char *visible_state_string(enum widget_visible_state state)
{
	switch (state) {
	case WIDGET_SHOW:
		return "Show";
	case WIDGET_HIDE:
		return "Hide";
	case WIDGET_HIDE_WITH_PAUSE:
		return "Paused";
	default:
		break;
	}

	return "Unknown";
}

static Eina_Bool inst_list_cb(void *info)
{
	FILE *fp;
	char *pkgname;
	struct pkg_info *pkg;
	Eina_List *l;
	Eina_List *inst_list;
	struct inst_info *inst;

	pkgname = widget_mgr_data(info);
	if (!pkgname) {
		return ECORE_CALLBACK_CANCEL;
	}

	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		ErrPrint("Invalid fp\n");
		widget_mgr_close_fifo(info);
		free(pkgname);
		return ECORE_CALLBACK_CANCEL;
	}

	if (!package_is_widget_pkgname(pkgname)) {
		ErrPrint("Invalid package name\n");
		free(pkgname);
		goto close_out;
	}

	pkg = package_find(pkgname);
	free(pkgname);
	if (!pkg) {
		ErrPrint("Package is not exists\n");
		goto close_out;
	}

	inst_list = package_instance_list(pkg);
	EINA_LIST_FOREACH(inst_list, l, inst) {
		fprintf(fp, "%s %s %s %s %lf %s %d %d\n",
				instance_id(inst),
				buffer_handler_id(instance_widget_buffer(inst)),
				instance_cluster(inst),
				instance_category(inst),
				instance_period(inst),
				visible_state_string(instance_visible_state(inst)),
				instance_widget_width(inst),
				instance_widget_height(inst));
	}

close_out:
	fprintf(fp, "EOD\n");
	widget_mgr_close_fifo(info);

	return ECORE_CALLBACK_CANCEL;
}

static struct packet *widget_mgr_inst_list(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	char *dup_pkgname;
	struct widget_mgr *info;

	if (packet_get(packet, "s", &pkgname) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = widget_mgr_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	dup_pkgname = strdup(pkgname);
	if (!dup_pkgname) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	widget_mgr_set_data(info, dup_pkgname);
	inst_list_cb(info);

out:
	return NULL;
}

static Eina_Bool pkg_list_cb(void *info)
{
	FILE *fp;
	Eina_List *l;
	Eina_List *list;
	Eina_List *inst_list;
	struct pkg_info *pkg;
	struct slave_node *slave;
	const char *slavename;
	pid_t pid;

	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		DbgPrint("Failed to open a pipe\n");
		widget_mgr_close_fifo(info);
		return ECORE_CALLBACK_CANCEL;
	}

	list = (Eina_List *)package_list();
	EINA_LIST_FOREACH(list, l, pkg) {
		slave = package_slave(pkg);

		if (slave) {
			slavename = slave_name(slave);
			pid = slave_pid(slave);
		} else {
			pid = (pid_t)-1;
			slavename = "";
		}

		inst_list = (Eina_List *)package_instance_list(pkg);
		fprintf(fp, "%d %s %s %s %d %d %d\n",
				pid,
				strlen(slavename) ? slavename : "(none)",
				package_name(pkg),
				package_abi(pkg),
				package_refcnt(pkg),
				package_fault_count(pkg),
				eina_list_count(inst_list)
			   );
		DbgPrint("%d %s %s %s %d %d %d\n",
				pid,
				strlen(slavename) ? slavename : "(none)",
				package_name(pkg),
				package_abi(pkg),
				package_refcnt(pkg),
				package_fault_count(pkg),
				eina_list_count(inst_list)
				);
	}

	fprintf(fp, "EOD\n");
	DbgPrint("EOD\n");
	widget_mgr_close_fifo(info);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *widget_mgr_pkg_list(pid_t pid, int handle, const struct packet *packet)
{
	struct widget_mgr *info;
	double timestamp;

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	DbgPrint("Package List: %lf\n", timestamp);

	info = widget_mgr_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	pkg_list_cb(info);
out:
	return NULL;
}

static struct packet *widget_mgr_slave_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	char *pkgname;
	char *id;
	int cmd;
	int value;
	int packet_cmd;
	struct widget_mgr *mgr;
	struct inst_info *inst;
	struct pkg_info *info;
	struct packet *ctrl_packet;

	if (packet_get(packet, "ssii", &pkgname, &id, &cmd, &value) != 4) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	mgr = widget_mgr_find_by_pid(pid);
	if (!mgr) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	switch (cmd) {
	case WIDGET_CTRL_MODE_DUMP_FRAME:
		inst = package_find_instance_by_id(pkgname, id);
		if (!inst) {
			ErrPrint("Instance not found: %s\n", id);
			goto out;
		}

		info = instance_package(inst);
		if (!info) {
			ErrPrint("Unable to get pkginfo\n");
			goto out;
		}

		packet_cmd = CMD_CTRL_MODE;
		ctrl_packet = packet_create_noack((const char *)&packet_cmd, "ssii", pkgname, id, cmd, value);
		if (ctrl_packet) {
			if (slave_rpc_request_only(package_slave(info), package_name(info), ctrl_packet, 0) < 0) {
				ErrPrint("Failed to send request\n");
			}
		}

		DbgPrint("Dump frames: %d\n", value);
		break;
	default:
		ErrPrint("Unknown command\n");
		break;
	}

out:
	return NULL;
}

static Eina_Bool pkg_ctrl_rmpack_cb(void *info)
{
	FILE *fp;
	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		widget_mgr_close_fifo(info);
		return ECORE_CALLBACK_CANCEL;
	}

	fprintf(fp, "%d\n", ENOSYS);
	fprintf(fp, "EOD\n");
	widget_mgr_close_fifo(info);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool pkg_ctrl_rminst_cb(void *info)
{
	FILE *fp;

	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		widget_mgr_close_fifo(info);
		return ECORE_CALLBACK_CANCEL;
	}

	fprintf(fp, "%ld\n", (long)widget_mgr_data(info));
	fprintf(fp, "EOD\n");
	widget_mgr_close_fifo(info);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool pkg_ctrl_faultinst_cb(void *info)
{
	FILE *fp;

	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		widget_mgr_close_fifo(info);
		return ECORE_CALLBACK_CANCEL;
	}

	fprintf(fp, "%ld\n", (long)widget_mgr_data(info));
	fprintf(fp, "EOD\n");
	widget_mgr_close_fifo(info);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *widget_mgr_pkg_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	struct widget_mgr *info;
	char *cmd;
	char *pkgname;
	char *id;

	if (packet_get(packet, "sss", &cmd, &pkgname, &id) != 3) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = widget_mgr_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	if (!strcmp(cmd, "rmpack")) {
		pkg_ctrl_rmpack_cb(info);
	} else if (!strcmp(cmd, "rminst")) {
		struct inst_info *inst;
		inst = package_find_instance_by_id(pkgname, id);
		if (!inst) {
			widget_mgr_set_data(info, (void *)ENOENT);
		} else {
			(void)instance_destroy(inst, WIDGET_DESTROY_TYPE_DEFAULT);
			widget_mgr_set_data(info, (void *)0);
		}

		pkg_ctrl_rminst_cb(info);
	} else if (!strcmp(cmd, "faultinst")) {
		struct inst_info *inst;
		inst = package_find_instance_by_id(pkgname, id);
		if (!inst) {
			widget_mgr_set_data(info, (void *)ENOENT);
		} else {
			struct pkg_info *pkg;

			pkg = instance_package(inst);
			if (!pkg) {
				widget_mgr_set_data(info, (void *)EFAULT);
			} else {
				(void)package_faulted(pkg, 1);
				widget_mgr_set_data(info, (void *)0);
			}
		}

		pkg_ctrl_faultinst_cb(info);
	}

out:
	return NULL;
}

static Eina_Bool master_ctrl_cb(void *info)
{
	FILE *fp;

	widget_mgr_open_fifo(info);
	fp = widget_mgr_fifo(info);
	if (!fp) {
		widget_mgr_close_fifo(info);
		return ECORE_CALLBACK_CANCEL;
	}
	fprintf(fp, "%ld\nEOD\n", (long)widget_mgr_data(info));
	widget_mgr_close_fifo(info);

	return ECORE_CALLBACK_CANCEL;
}

static struct packet *widget_mgr_master_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	struct widget_mgr *info;
	char *cmd;
	char *var;
	char *val;
	long ret = WIDGET_ERROR_INVALID_PARAMETER;

	if (packet_get(packet, "sss", &cmd, &var, &val) != 3) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = widget_mgr_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	if (!strcasecmp(var, "debug")) {
		if (!strcasecmp(cmd, "set")) {
			g_conf.debug_mode = !strcasecmp(val, "on");
		} else if (!strcasecmp(cmd, "get")) {
		}
		ret = g_conf.debug_mode;
	} else if (!strcasecmp(var, "slave_max_load")) {
		if (!strcasecmp(cmd, "set")) {
			g_conf.slave_max_load = atoi(val);
		} else if (!strcasecmp(cmd, "get")) {
		}
		ret = g_conf.slave_max_load;
	}

	widget_mgr_set_data(info, (void *)ret);
	master_ctrl_cb(info);

out:
	return NULL;
}

static struct method s_info_table[] = {
	{
		.cmd = CMD_STR_INFO_HELLO,
		.handler = widget_mgr_hello,
	},
	{
		.cmd = CMD_STR_INFO_SLAVE_LIST,
		.handler = widget_mgr_slave_list,
	},
	{
		.cmd = CMD_STR_INFO_PKG_LIST,
		.handler = widget_mgr_pkg_list,
	},
	{
		.cmd = CMD_STR_INFO_INST_LIST,
		.handler = widget_mgr_inst_list,
	},
	{
		.cmd = CMD_STR_INFO_SLAVE_CTRL,
		.handler = widget_mgr_slave_ctrl,
	},
	{
		.cmd = CMD_STR_INFO_PKG_CTRL,
		.handler = widget_mgr_pkg_ctrl,
	},
	{
		.cmd = CMD_STR_INFO_MASTER_CTRL,
		.handler = widget_mgr_master_ctrl,
	},
	{
		.cmd = NULL,
		.handler = NULL,
	},
};

static struct method s_client_table[] = {
	{
		.cmd = CMD_STR_GBAR_MOUSE_MOVE,
		.handler = client_gbar_mouse_move, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_MOVE,
		.handler = client_widget_mouse_move, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_DOWN,
		.handler = client_gbar_mouse_down, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_UP,
		.handler = client_gbar_mouse_up, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_DOWN,
		.handler = client_widget_mouse_down, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_UP,
		.handler = client_widget_mouse_up, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_ENTER,
		.handler = client_gbar_mouse_enter, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_LEAVE,
		.handler = client_gbar_mouse_leave, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_ENTER,
		.handler = client_widget_mouse_enter, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_LEAVE,
		.handler = client_widget_mouse_leave, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_ON_SCROLL,
		.handler = client_widget_mouse_on_scroll,
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_OFF_SCROLL,
		.handler = client_widget_mouse_off_scroll,
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_ON_SCROLL,
		.handler = client_gbar_mouse_on_scroll,
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_OFF_SCROLL,
		.handler = client_gbar_mouse_off_scroll,
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_ON_HOLD,
		.handler = client_widget_mouse_on_hold,
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_OFF_HOLD,
		.handler = client_widget_mouse_off_hold,
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_ON_HOLD,
		.handler = client_gbar_mouse_on_hold,
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_OFF_HOLD,
		.handler = client_gbar_mouse_off_hold,
	},
	{
		.cmd = CMD_STR_CLICKED,
		.handler = client_clicked, /*!< pid, pkgname, filename, event, timestamp, x, y, ret */
	},
	{
		.cmd = CMD_STR_TEXT_SIGNAL,
		.handler = client_text_signal, /* pid, pkgname, filename, signal_name, source, s, sy, ex, ey, ret */
	},
	{
		.cmd = CMD_STR_DELETE,
		.handler = client_delete, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = CMD_STR_RESIZE,
		.handler = client_resize, /* pid, pkgname, filename, w, h, ret */
	},
	{
		.cmd = CMD_STR_NEW,
		.handler = client_new, /* pid, timestamp, pkgname, content, cluster, category, period, ret */
	},
	{
		.cmd = CMD_STR_SET_PERIOD,
		.handler = client_set_period, /* pid, pkgname, filename, period, ret, period */
	},
	{
		.cmd = CMD_STR_CHANGE_GROUP,
		.handler = client_change_group, /* pid, pkgname, filename, cluster, category, ret */
	},
	{
		.cmd = CMD_STR_GBAR_MOVE,
		.handler = client_gbar_move, /* pkgname, id, x, y */
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_HL,
		.handler = client_gbar_access_hl,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_ACTIVATE,
		.handler = client_gbar_access_activate,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_ACTION,
		.handler = client_gbar_access_action,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_SCROLL,
		.handler = client_gbar_access_scroll,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_VALUE_CHANGE,
		.handler = client_gbar_access_value_change,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_MOUSE,
		.handler = client_gbar_access_mouse,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_BACK,
		.handler = client_gbar_access_back,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_OVER,
		.handler = client_gbar_access_over,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_READ,
		.handler = client_gbar_access_read,
	},
	{
		.cmd = CMD_STR_GBAR_ACCESS_ENABLE,
		.handler = client_gbar_access_enable,
	},

	{
		.cmd = CMD_STR_WIDGET_ACCESS_HL,
		.handler = client_widget_access_hl,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_ACTIVATE,
		.handler = client_widget_access_activate,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_ACTION,
		.handler = client_widget_access_action,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_SCROLL,
		.handler = client_widget_access_scroll,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_VALUE_CHANGE,
		.handler = client_widget_access_value_change,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_MOUSE,
		.handler = client_widget_access_mouse,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_BACK,
		.handler = client_widget_access_back,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_OVER,
		.handler = client_widget_access_over,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_READ,
		.handler = client_widget_access_read,
	},
	{
		.cmd = CMD_STR_WIDGET_ACCESS_ENABLE,
		.handler = client_widget_access_enable,
	},
	{
		.cmd = CMD_STR_WIDGET_KEY_DOWN,
		.handler = client_widget_key_down,
	},
	{
		.cmd = CMD_STR_WIDGET_KEY_UP,
		.handler = client_widget_key_up,
	},
	{
		.cmd = CMD_STR_WIDGET_KEY_FOCUS_IN,
		.handler = client_widget_key_focus_in,
	},
	{
		.cmd = CMD_STR_WIDGET_KEY_FOCUS_OUT,
		.handler = client_widget_key_focus_out,
	},
	{
		.cmd = CMD_STR_GBAR_KEY_DOWN,
		.handler = client_gbar_key_down,
	},
	{
		.cmd = CMD_STR_GBAR_KEY_UP,
		.handler = client_gbar_key_up,
	},
	{
		.cmd = CMD_STR_GBAR_KEY_FOCUS_IN,
		.handler = client_gbar_key_focus_in,
	},
	{
		.cmd = CMD_STR_GBAR_KEY_FOCUS_OUT,
		.handler = client_gbar_key_focus_out,
	},
	{
		.cmd = CMD_STR_UPDATE_MODE,
		.handler = client_update_mode,
	},
	// Cut HERE. Above list must be sync'd with provider list.

	{
		.cmd = CMD_STR_WIDGET_MOUSE_SET,
		.handler = client_widget_mouse_set,
	},
	{
		.cmd = CMD_STR_WIDGET_MOUSE_UNSET,
		.handler = client_widget_mouse_unset,
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_SET,
		.handler = client_gbar_mouse_set,
	},
	{
		.cmd = CMD_STR_GBAR_MOUSE_UNSET,
		.handler = client_gbar_mouse_unset,
	},
	{
		.cmd = CMD_STR_ORIENTATION,
		.handler = client_change_orientation,
	},
	{
		.cmd = CMD_STR_CHANGE_VISIBILITY,
		.handler = client_change_visibility,
	},
	{
		.cmd = CMD_STR_WIDGET_ACQUIRE_PIXMAP,
		.handler = client_widget_acquire_pixmap,
	},
	{
		.cmd = CMD_STR_WIDGET_RELEASE_PIXMAP,
		.handler = client_widget_release_pixmap,
	},
	{
		.cmd = CMD_STR_GBAR_ACQUIRE_PIXMAP,
		.handler = client_gbar_acquire_pixmap,
	},
	{
		.cmd = CMD_STR_GBAR_RELEASE_PIXMAP,
		.handler = client_gbar_release_pixmap,
	},
	{
		.cmd = CMD_STR_ACQUIRE,
		.handler = client_acquire, /*!< pid, ret */
	},
	{
		.cmd = CMD_STR_RELEASE,
		.handler = cilent_release, /*!< pid, ret */
	},
	{
		.cmd = CMD_STR_PINUP_CHANGED,
		.handler = client_pinup_changed, /* pid, pkgname, filename, pinup, ret */
	},
	{
		.cmd = CMD_STR_CREATE_GBAR,
		.handler = client_create_gbar, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = CMD_STR_DESTROY_GBAR,
		.handler = client_destroy_gbar, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = CMD_STR_ACTIVATE_PACKAGE,
		.handler = client_activate_package, /* pid, pkgname, ret */
	},
	{
		.cmd = CMD_STR_SUBSCRIBE, /* pid, cluster, sub-cluster */
		.handler = client_subscribed_group,
	},
	{
		.cmd = CMD_STR_UNSUBSCRIBE, /* pid, cluster, sub-cluster */
		.handler = client_unsubscribed_group,
	},
	{
		.cmd = CMD_STR_DELETE_CLUSTER,
		.handler = client_delete_cluster,
	},
	{
		.cmd = CMD_STR_DELETE_CATEGORY,
		.handler = client_delete_category,
	},
	{
		.cmd = CMD_STR_REFRESH_GROUP,
		.handler = client_refresh_group,
	},
	{
		.cmd = CMD_STR_UPDATE,
		.handler = client_update,
	},

	{
		.cmd = CMD_STR_WIDGET_KEY_SET,
		.handler = client_widget_key_set,
	},
	{
		.cmd = CMD_STR_WIDGET_KEY_UNSET,
		.handler = client_widget_key_unset,
	},

	{
		.cmd = CMD_STR_GBAR_KEY_SET,
		.handler = client_gbar_key_set,
	},
	{
		.cmd = CMD_STR_GBAR_KEY_UNSET,
		.handler = client_gbar_key_unset,
	},

	{
		.cmd = CMD_STR_CLIENT_PAUSED,
		.handler = client_pause_request,
	},
	{
		.cmd = CMD_STR_CLIENT_RESUMED,
		.handler = client_resume_request,
	},
	{
		.cmd = CMD_STR_WIDGET_ACQUIRE_XPIXMAP,
		.handler = client_widget_acquire_xpixmap,
	},
	{
		.cmd = CMD_STR_GBAR_ACQUIRE_XPIXMAP,
		.handler = client_gbar_acquire_xpixmap,
	},
	{
		.cmd = CMD_STR_SUBSCRIBE_CATEGORY,
		.handler = client_subscribed_category,
	},
	{
		.cmd = CMD_STR_UNSUBSCRIBE_CATEGORY,
		.handler = client_unsubscribed_category,
	},

    {
        .cmd = CMD_STR_HELLO_SYNC,
        .handler = slave_hello_sync, /* slave_name, ret */
    },
    {
        .cmd = CMD_STR_HELLO_SYNC_PREPARE,
        .handler = slave_hello_sync_prepare, /* timestamp */
    },

	{
		.cmd = NULL,
		.handler = NULL,
	},
};

static struct method s_service_table[] = {
	{
		.cmd = CMD_STR_SERVICE_UPDATE,
		.handler = service_update,
	},
	{
		.cmd = CMD_STR_SERVICE_CHANGE_PERIOD,
		.handler = service_change_period,
	},
	{
		.cmd = CMD_STR_SERVICE_INST_CNT,
		.handler = service_instance_count,
	},
	{
		.cmd = CMD_STR_MONITOR_REGISTER,
		.handler = monitor_register,
	},
	{
		.cmd = CMD_STR_MONITOR_UNREGISTER,
		.handler = monitor_unregister,
	},
	{
		.cmd = CMD_STR_SERVICE_GET_CONTENT,
		.handler = service_get_content,
	},
	{
		.cmd = CMD_STR_SERVICE_GET_INST_LIST,
		.handler = service_get_inst_list,
	},
	{
		.cmd = NULL,
		.handler = NULL,
	},
};

static struct method s_slave_table[] = {
	{
		.cmd = CMD_STR_UPDATED,
		.handler = slave_updated, /* slave_name, pkgname, filename, width, height, ret */
	},
	{
		.cmd = CMD_STR_DESC_UPDATED,
		.handler = slave_desc_updated, /* slave_name, pkgname, filename, decsfile, ret */
	},
	{
		.cmd = CMD_STR_EXTRA_UPDATED,
		.handler = slave_extra_updated,
	},
	{
		.cmd = CMD_STR_EXTRA_INFO,
		.handler = slave_extra_info, /* slave_name, pkgname, filename, priority, content_info, title, icon, name */
	},
	{
		.cmd = CMD_STR_DELETED,
		.handler = slave_deleted, /* slave_name, pkgname, filename, ret */
	},
	{
		.cmd = CMD_STR_FAULTED,
		.handler = slave_faulted, /* slave_name, pkgname, id, funcname */
	},
	{
		.cmd = CMD_STR_SCROLL,
		.handler = slave_hold_scroll, /* slave_name, pkgname, id, seize */
	},

	{
		.cmd = CMD_STR_WIDGET_UPDATE_BEGIN,
		.handler = slave_widget_update_begin,
	},
	{
		.cmd = CMD_STR_WIDGET_UPDATE_END,
		.handler = slave_widget_update_end,
	},
	{
		.cmd = CMD_STR_GBAR_UPDATE_BEGIN,
		.handler = slave_gbar_update_begin,
	},
	{
		.cmd = CMD_STR_GBAR_UPDATE_END,
		.handler = slave_gbar_update_end,
	},

	{
		.cmd = CMD_STR_ACCESS_STATUS,
		.handler = slave_access_status,
	},
	{
		.cmd = CMD_STR_KEY_STATUS,
		.handler = slave_key_status,
	},
	{
		.cmd = CMD_STR_CLOSE_GBAR,
		.handler = slave_close_gbar,
	},

	{
		.cmd = CMD_STR_CALL,
		.handler = slave_call, /* slave_name, pkgname, filename, function, ret */
	},
	{
		.cmd = CMD_STR_RET,
		.handler = slave_ret, /* slave_name, pkgname, filename, function, ret */
	},
	{
		.cmd = CMD_STR_ACQUIRE_BUFFER,
		.handler = slave_acquire_buffer, /* slave_name, id, w, h, size, - out - type, shmid */
	},
	{
		.cmd = CMD_STR_RESIZE_BUFFER,
		.handler = slave_resize_buffer,
	},
	{
		.cmd = CMD_STR_RELEASE_BUFFER,
		.handler = slave_release_buffer, /* slave_name, id - ret */
	},
	{
		.cmd = CMD_STR_HELLO,
		.handler = slave_hello, /* slave_name, ret */
	},
	{
		.cmd = CMD_STR_PING,
		.handler = slave_ping, /* slave_name, ret */
	},
	{
		.cmd = CMD_STR_CTRL,
		.handler = slave_ctrl, /* control bits */
	},

	{
		.cmd = CMD_STR_ACQUIRE_XBUFFER,
		.handler = slave_acquire_extra_buffer,
	},
	{
		.cmd = CMD_STR_RELEASE_XBUFFER,
		.handler = slave_release_extra_buffer,
	},

    {
        .cmd = CMD_STR_HELLO_SYNC,
        .handler = slave_hello_sync, /* slave_name, ret */
    },
    {
        .cmd = CMD_STR_HELLO_SYNC_PREPARE,
        .handler = slave_hello_sync_prepare, /* timestamp */
    },

	{
		.cmd = NULL,
		.handler = NULL,
	},
};

HAPI int server_init(void)
{
	if (unlink(INFO_SOCKET) < 0) {
		ErrPrint("unlink info: %d\n", errno);
	}

	s_info.info_fd = com_core_packet_server_init(INFO_SOCKET, s_info_table);
	if (s_info.info_fd < 0) {
		ErrPrint("Failed to create a info socket\n");
	}

	s_info.slave_fd = com_core_packet_server_init_with_permission("sdlocal://"SLAVE_SOCKET, s_slave_table, NULL);
	if (s_info.slave_fd < 0) {
		ErrPrint("Failed to create a slave socket\n");
	}

	s_info.client_fd = com_core_packet_server_init_with_permission("sdlocal://"CLIENT_SOCKET, s_client_table, NULL);
	if (s_info.client_fd < 0) {
		ErrPrint("Failed to create a client socket\n");
	}

	s_info.service_fd = com_core_packet_server_init_with_permission("sdlocal://"SERVICE_SOCKET, s_service_table, NULL);
	if (s_info.service_fd < 0) {
		ErrPrint("Faild to create a service socket\n");
	}

	if (chmod(INFO_SOCKET, 0600) < 0) {
		ErrPrint("chmod info: %d\n", errno);
	}

	return 0;
}

HAPI int server_fini(void)
{
	if (s_info.info_fd > 0) {
		com_core_packet_server_fini(s_info.info_fd);
		s_info.info_fd = -1;
	}

	if (s_info.slave_fd > 0) {
		com_core_packet_server_fini(s_info.slave_fd);
		s_info.slave_fd = -1;
	}

	if (s_info.client_fd > 0) {
		com_core_packet_server_fini(s_info.client_fd);
		s_info.client_fd = -1;
	}

	if (s_info.service_fd > 0) {
		com_core_packet_server_fini(s_info.service_fd);
		s_info.service_fd = -1;
	}

	return 0;
}

/* End of a file */
