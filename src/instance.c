#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <dlog.h>
#include <Ecore_Evas.h>
#include <Eina.h>
#include <gio/gio.h>
#include <Ecore.h>

#include <packet.h>
#include <com-core_packet.h>

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
#include "fb.h"
#include "setting.h"

int errno;

struct set_pinup_cbdata {
	struct inst_info *inst;
	int pinup;
};

struct resize_cbdata {
	struct inst_info *inst;
	int w;
	int h;
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

struct inst_info {
	struct pkg_info *info;

	enum instance_state state; /*!< Represents current state */
	enum instance_state requested_state; /*!< Only ACTIVATED | DESTROYED is acceptable */
	int changing_state;

	char *id;
	double timestamp;

	char *content;
	char *cluster;
	char *category;
	char *title;
	int is_pinned_up;

	enum livebox_visible_state visible;

	struct {
		int width;
		int height;
		double priority;

		union {
			struct script_info *script;
			struct buffer_info *buffer;
		} canvas;

		int auto_launch;
	} lb;

	struct {
		int width;
		int height;

		union {
			struct script_info *script;
			struct buffer_info *buffer;
		} canvas;

		int is_opened_for_reactivate;
		int need_to_send_close_event;
	} pd;

	int timeout;
	double period;

	struct client_node *client; /*!< Owner - creator */
	Eina_List *client_list; /*!< Viewer list */
	int refcnt;

	Ecore_Timer *update_timer; /*!< Only used for secured livebox */
};

#define CLIENT_SEND_EVENT(instance, packet)	((instance)->client ? client_rpc_async_request((instance)->client, (packet)) : client_rpc_broadcast((instance), (packet)))

static inline int pause_livebox(struct inst_info *inst)
{
	struct packet *packet;

	packet = packet_create_noack("lb_pause", "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

/*! \TODO Wake up the freeze'd timer */
static inline int resume_livebox(struct inst_info *inst)
{
	struct packet *packet;

	packet = packet_create_noack("lb_resume", "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

static inline int instance_recover_visible_state(struct inst_info *inst)
{
	int ret;

	switch (inst->visible) {
	case LB_SHOW:
	case LB_HIDE:
		instance_thaw_updator(inst);

		ret = 0;
		break;
	case LB_HIDE_WITH_PAUSE:
		ret = pause_livebox(inst);

		instance_freeze_updator(inst);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	DbgPrint("Visible state is recovered to %d\n", ret);
	return ret;
}

int instance_unicast_created_event(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;
	enum lb_type lb_type;
	enum pd_type pd_type;
	const char *lb_file;
	const char *pd_file;

	if (!client) {
		client = inst->client;
		if (!client)
			return 0;
	}

	lb_type = package_lb_type(inst->info);
	pd_type = package_pd_type(inst->info);

	if (lb_type == LB_TYPE_SCRIPT)
		lb_file = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		lb_file = buffer_handler_id(inst->lb.canvas.buffer);
	else
		lb_file = "";

	if (pd_type == PD_TYPE_SCRIPT)
		pd_file = fb_id(script_handler_fb(inst->pd.canvas.script));
	else if (pd_type == PD_TYPE_BUFFER)
		pd_file = buffer_handler_id(inst->pd.canvas.buffer);
	else
		pd_file = "";

	packet = packet_create_noack("created", "dsssiiiissssidiiiiidsi",
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->lb.width, inst->lb.height,
			inst->pd.width, inst->pd.height,
			inst->cluster, inst->category,
			lb_file, pd_file,
			inst->lb.auto_launch,
			inst->lb.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			lb_type, pd_type,
			inst->period, inst->title,
			inst->is_pinned_up);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return client_rpc_async_request(client, packet);
}

int instance_broadcast_created_event(struct inst_info *inst)
{
	struct packet *packet;
	enum lb_type lb_type;
	enum pd_type pd_type;
	const char *lb_file;
	const char *pd_file;

	lb_type = package_lb_type(inst->info);
	pd_type = package_pd_type(inst->info);

	if (lb_type == LB_TYPE_SCRIPT)
		lb_file = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		lb_file = buffer_handler_id(inst->lb.canvas.buffer);
	else
		lb_file = "";

	if (pd_type == PD_TYPE_SCRIPT)
		pd_file = fb_id(script_handler_fb(inst->pd.canvas.script));
	else if (pd_type == PD_TYPE_BUFFER)
		pd_file = buffer_handler_id(inst->pd.canvas.buffer);
	else
		pd_file = "";

	packet = packet_create_noack("created", "dsssiiiissssidiiiiidsi", 
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->lb.width, inst->lb.height,
			inst->pd.width, inst->pd.height,
			inst->cluster, inst->category,
			lb_file, pd_file,
			inst->lb.auto_launch,
			inst->lb.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			lb_type, pd_type,
			inst->period, inst->title,
			inst->is_pinned_up);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return CLIENT_SEND_EVENT(inst, packet);
}

int instance_unicast_deleted_event(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;

	if (!client) {
		client = inst->client;
		if (!client)
			return -EINVAL;
	}

	packet = packet_create_noack("deleted", "ssd", package_name(inst->info), inst->id, inst->timestamp);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}
		
	return client_rpc_async_request(client, packet);
}

int instance_broadcast_deleted_event(struct inst_info *inst)
{
	struct packet *packet;

	packet = packet_create_noack("deleted", "ssd", package_name(inst->info), inst->id, inst->timestamp);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}
		
	return CLIENT_SEND_EVENT(inst, packet);
}

static int client_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	instance_destroy(inst);
	return 0;
}

static int send_pd_destroyed_to_client(struct inst_info *inst, int status)
{
	struct packet *packet;

	packet = packet_create_noack("pd_destroyed", "ssi", package_name(inst->info), inst->id, status);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return CLIENT_SEND_EVENT(inst, packet);
}

static inline void destroy_instance(struct inst_info *inst)
{
	struct pkg_info *pkg;
	enum lb_type lb_type;
	enum pd_type pd_type;
	struct slave_node *slave;

	pkg = inst->info;

	lb_type = package_lb_type(pkg);
	pd_type = package_pd_type(pkg);
	slave = package_slave(inst->info);

	if (inst->pd.need_to_send_close_event)
		send_pd_destroyed_to_client(inst, 0);

	if (lb_type == LB_TYPE_SCRIPT) {
		script_handler_unload(inst->lb.canvas.script, 0);
		script_handler_destroy(inst->lb.canvas.script);
	} else if (lb_type == LB_TYPE_BUFFER) {
		buffer_handler_unload(inst->lb.canvas.buffer);
		buffer_handler_destroy(inst->lb.canvas.buffer);
	}

	if (pd_type == PD_TYPE_SCRIPT) {
		script_handler_unload(inst->pd.canvas.script, 1);
		script_handler_destroy(inst->pd.canvas.script);
	} else if (pd_type == PD_TYPE_BUFFER) {
		buffer_handler_unload(inst->pd.canvas.buffer);
		buffer_handler_destroy(inst->pd.canvas.buffer);
	}

	if (inst->client) {
		client_event_callback_del(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst);
		client_unref(inst->client);
	}

	if (inst->update_timer)
		ecore_timer_del(inst->update_timer);

	free(inst->category);
	free(inst->cluster);
	free(inst->content);
	free(inst->title);
	util_unlink(inst->id);
	free(inst->id);
	package_del_instance(inst->info, inst);
	free(inst);

	slave = slave_unload_instance(slave);
	DbgPrint("Instance is destroyed (%p), slave(%p)\n", inst, slave);
}

static Eina_Bool update_timer_cb(void *data)
{
	struct inst_info *inst = (struct inst_info *)data;

	DbgPrint("Update instance %s (%s) %s/%s\n", package_name(inst->info), inst->id, inst->cluster, inst->category);
	slave_rpc_request_update(package_name(inst->info), inst->id, inst->cluster, inst->category);
	return ECORE_CALLBACK_RENEW;
}

static inline int fork_package(struct inst_info *inst, const char *pkgname)
{
	struct pkg_info *info;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("%s is not found\n", pkgname);
		return -ENOENT;
	}

	inst->lb.auto_launch = package_auto_launch(info);

	inst->pd.width = package_pd_width(info);
	inst->pd.height = package_pd_height(info);

	inst->timeout = package_timeout(info);
	inst->period = package_period(info);

	inst->info = info;

	if (package_secured(info)) {
		DbgPrint("Register the update timer for secured livebox [%s]\n", pkgname);
		inst->update_timer = ecore_timer_add(inst->period, update_timer_cb, inst);
		if (!inst->update_timer)
			ErrPrint("Failed to add an update timer for instance %s\n", inst->id);
		else
			ecore_timer_freeze(inst->update_timer); /* Freeze the update timer as default */
	}

	return 0;
}

struct inst_info *instance_create(struct client_node *client, double timestamp, const char *pkgname, const char *content, const char *cluster, const char *category, double period)
{
	struct inst_info *inst;
	char id[BUFSIZ];

	inst = calloc(1, sizeof(*inst));
	if (!inst) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	inst->timestamp = timestamp;

	snprintf(id, sizeof(id), SCHEMA_FILE "%s%s_%d_%lf.png", IMAGE_PATH, pkgname, client_pid(client), inst->timestamp);
	inst->id = strdup(id);
	if (!inst->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst);
		return NULL;
	}

	inst->content = strdup(content);
	if (!inst->content) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst->id);
		free(inst);
		return NULL;
	}

	inst->cluster = strdup(cluster);
	if (!inst->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst->content);
		free(inst->id);
		free(inst);
		return NULL;
	}

	inst->category = strdup(category);
	if (!inst->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst->cluster);
		free(inst->content);
		free(inst->id);
		free(inst);
		return NULL;
	}

	inst->title = strdup(DEFAULT_TITLE); /*!< Use the DEFAULT Title "" */
	if (!inst->title) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst->category);
		free(inst->cluster);
		free(inst->content);
		free(inst->id);
		free(inst);
		return NULL;
	}

	if (fork_package(inst, pkgname) < 0) {
		free(inst->title);
		free(inst->category);
		free(inst->cluster);
		free(inst->content);
		free(inst->id);
		free(inst);
		return NULL;
	}

	if (client) {
		inst->client = client_ref(client);
		client_event_callback_add(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst);
	}

	inst->state = INST_INIT;
	inst->requested_state = INST_INIT;
	instance_ref(inst);

	if (package_add_instance(inst->info, inst) < 0) {
		instance_state_reset(inst);
		instance_destroy(inst);
		return NULL;
	}

	slave_load_instance(package_slave(inst->info));

	if (instance_activate(inst) < 0) {
		instance_state_reset(inst);
		instance_destroy(inst);
		inst = NULL;
	}

	return inst;
}

struct inst_info * instance_ref(struct inst_info *inst)
{
	if (!inst)
		return NULL;

	inst->refcnt++;
	return inst;
}

struct inst_info *instance_unref(struct inst_info *inst)
{
	if (!inst)
		return NULL;

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
	struct pkg_info *info;
	int ret;

	/*!
	 * \note
	 * In this callback, we cannot trust the "client" information.
	 * It could be cleared before reach to here.
	 */

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * The instance_reload will care this.
		 * And it will be called from the slave activate callback.
		 */
		inst->changing_state = 0;
		instance_unref(inst);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid argument\n");
		inst->changing_state = 0;
		instance_unref(inst);
		return;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		inst->changing_state = 0;
		instance_unref(inst);
		return;
	}

	switch (ret) {
	case 0:
		/*!
		 * \note
		 * Successfully unloaded
		 */
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			DbgPrint("REQ: ACTIVATED\n");
			instance_state_reset(inst);
			instance_reactivate(inst);
			break;
		case INST_DESTROYED:
			info = inst->info;
			instance_broadcast_deleted_event(inst);
			instance_state_reset(inst);
			instance_destroy(inst);
			DbgPrint("== %s\n", package_name(info));
		default:
			/*!< Unable to reach here */
			break;
		}

		break;
	case -EINVAL:
		/*!
		 * \note
		 * Slave has no instance of this package.
		 */
	case -ENOENT:
		/*!
		 * \note
		 * This instance's previous state is only can be the INST_ACTIVATED.
		 * So we should care the slave_unload_instance from here.
		 * And we should send notification to clients, about this is deleted.
		 */
		/*!
		 * \note
		 * Slave has no instance of this.
		 * In this case, ignore the requested_state
		 * Because, this instance is already met a problem.
		 */
	default:
		/*!
		 * \note
		 * Failed to unload this instance.
		 * This is not possible, slave will always return -ENOENT, -EINVAL, or 0.
		 * but care this exceptional case.
		 */
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(inst->info), ret);
		info = inst->info;
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

	inst->changing_state = 0;
	instance_unref(inst);
}

static void reactivate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	struct pkg_info *info;
	enum lb_type lb_type;
	enum pd_type pd_type;
	int ret;
	const char *content;
	const char *title;
	int is_pinned_up;

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
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
			ErrPrint("Heap: %s\n", strerror(errno));
			goto out;
		}

		free(inst->content);
		inst->content = tmp;

		DbgPrint("Update content info %s\n", tmp);
	}

	if (strlen(title)) {
		char *tmp;

		tmp = strdup(title);
		if (!tmp) {
			ErrPrint("Heap: %s\n", strerror(errno));
			goto out;
		}

		free(inst->title);
		inst->title = tmp;

		DbgPrint("Update title info %s\n", tmp);
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
			instance_destroy(inst);
			break;
		case INST_ACTIVATED:
			inst->is_pinned_up = is_pinned_up;
			info = inst->info;
			lb_type = package_lb_type(info);
			pd_type = package_pd_type(info);

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

			if (lb_type == LB_TYPE_SCRIPT && inst->lb.canvas.script)
				script_handler_load(inst->lb.canvas.script, 0);
			else if (lb_type == LB_TYPE_BUFFER && inst->lb.canvas.buffer)
				buffer_handler_load(inst->lb.canvas.buffer);

			if (pd_type == PD_TYPE_SCRIPT && inst->pd.canvas.script && inst->pd.is_opened_for_reactivate) {
				script_handler_load(inst->pd.canvas.script, 1);

				/*!
				 * \note
				 * We should to send a request to open a PD to slave.
				 * if we didn't send it, the slave will not recognize the state of a PD.
				 * We have to keep the view of PD seamless even if the livebox is reactivated.
				 * To do that, send open request from here.
				 */
				instance_slave_open_pd(inst);
			} else if (pd_type == PD_TYPE_BUFFER && inst->pd.canvas.buffer && inst->pd.is_opened_for_reactivate) {
				buffer_handler_load(inst->lb.canvas.buffer);

				/*!
				 * \note
				 * We should to send a request to open a PD to slave.
				 * if we didn't send it, the slave will not recognize the state of a PD.
				 * We have to keep the view of PD seamless even if the livebox is reactivated.
				 * To do that, send open request from here.
				 */
				instance_slave_open_pd(inst);
			}

			/*!
			 * \note
			 * After create an instance again,
			 * Send resize request to the livebox.
			 */
			instance_resize(inst, inst->lb.width, inst->lb.height);

			/*!
			 * \note
			 * This function will check the visiblity of a livebox and
			 * make decision whether it thaw the update timer or not.
			 */
			instance_recover_visible_state(inst);
		default:
			break;
		}
		break;
	default:
		info = inst->info;
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(info), ret);
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

out:
	inst->changing_state = 0;
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
		DbgPrint("Consuming a request of a dead process\n");
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
		if (util_free_space(IMAGE_PATH) > MINIMUM_SPACE) {
			struct inst_info *new_inst;
			new_inst = instance_create(inst->client, util_timestamp(), package_name(inst->info),
							inst->content, inst->cluster, inst->category,
							inst->period);
			if (!new_inst)
				ErrPrint("Failed to create a new instance\n");
		} else {
			ErrPrint("Not enough space\n");
		}
	case 0: /*!< normally created */
		/*!
		 * \note
		 * Anyway this instance is loaded to the slave,
		 * so just increase the loaded instance counter
		 * After that, do reset jobs.
		 */
		inst->state = INST_ACTIVATED;

		instance_set_lb_info(inst, w, h, priority, content, title);

		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_unicast_deleted_event(inst, NULL);
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		case INST_ACTIVATED:
		default:
			/*!
			 * \note
			 * LB should be created at the create time
			 */
			inst->is_pinned_up = is_pinned_up;
			if (package_lb_type(inst->info) == LB_TYPE_SCRIPT) {
				if (inst->lb.width == 0 && inst->lb.height == 0) {
					inst->lb.width = g_conf.size[0].width;
					inst->lb.height = g_conf.size[0].height;
				}

				inst->lb.canvas.script = script_handler_create(inst,
								package_lb_path(inst->info),
								package_lb_group(inst->info),
								inst->lb.width, inst->lb.height);

				if (!inst->lb.canvas.script)
					ErrPrint("Failed to create LB\n");
				else
					script_handler_load(inst->lb.canvas.script, 0);
			} else if (package_lb_type(inst->info) == LB_TYPE_BUFFER) {
				instance_create_lb_buffer(inst);
			}

			if (package_pd_type(inst->info) == PD_TYPE_SCRIPT) {
				if (inst->pd.width == 0 && inst->pd.height == 0) {
					inst->pd.width = package_pd_width(inst->info);
					inst->pd.height = package_pd_height(inst->info);
				}

				inst->pd.canvas.script = script_handler_create(inst,
								package_pd_path(inst->info),
								package_pd_group(inst->info),
								inst->pd.width, inst->pd.height);

				if (!inst->pd.canvas.script)
					ErrPrint("Failed to create PD\n");
			} else if (package_pd_type(inst->info) == PD_TYPE_BUFFER) {
				instance_create_pd_buffer(inst);
			}

			instance_broadcast_created_event(inst);

			instance_thaw_updator(inst);
			break;
		}
		break;
	default:
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(inst->info), ret);
		instance_unicast_deleted_event(inst, NULL);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

out:
	inst->changing_state = 0;
	instance_unref(inst);
}

int instance_create_pd_buffer(struct inst_info *inst)
{
	if (inst->pd.width == 0 && inst->pd.height == 0) {
		inst->pd.width = package_pd_width(inst->info);
		inst->pd.height = package_pd_height(inst->info);
	}

	if (!inst->pd.canvas.buffer) {
		enum buffer_type type;
		const char *env_type;

		type = BUFFER_TYPE_FILE;
		env_type = getenv("USE_SHM_FOR_LIVE_CONTENT");
		if (env_type) {
			if (!strcasecmp(env_type, "shm"))
				type = BUFFER_TYPE_SHM;
			else if (!strcasecmp(env_type, "pixmap"))
				type = BUFFER_TYPE_PIXMAP;
		}

		inst->pd.canvas.buffer = buffer_handler_create(inst, type, inst->pd.width, inst->pd.height, sizeof(int));
		if (!inst->pd.canvas.buffer)
			ErrPrint("Failed to create PD Buffer\n");
	}

	return !!inst->pd.canvas.buffer;
}

int instance_create_lb_buffer(struct inst_info *inst)
{
	if (inst->lb.width == 0 && inst->lb.height == 0) {
		inst->lb.width = g_conf.size[0].width;
		inst->lb.height = g_conf.size[0].height;
	}

	if (!inst->lb.canvas.buffer) {
		enum buffer_type type;
		const char *env_type;

		type = BUFFER_TYPE_FILE;
		env_type = getenv("USE_SHM_FOR_LIVE_CONTENT");
		if (env_type) {
			if (!strcasecmp(env_type, "shm"))
				type = BUFFER_TYPE_SHM;
			else if (!strcasecmp(env_type, "pixmap"))
				type = BUFFER_TYPE_PIXMAP;
		}
		/*!
		 * \note
		 * Slave doesn't call the acquire_buffer.
		 * In this case, create the buffer from here.
		 */
		inst->lb.canvas.buffer = buffer_handler_create(inst, type, inst->lb.width, inst->lb.height, sizeof(int));
		if (!inst->lb.canvas.buffer)
			ErrPrint("Failed to create LB\n");
	}

	return !!inst->lb.canvas.buffer;
}

int instance_destroyed(struct inst_info *inst)
{
	struct pkg_info *info;

	switch (inst->state) {
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		/*!
		 * \note
		 * No other clients know the existence of this instance,
		 * only who added this knows it.
		 * So send deleted event to only it.
		 */
		instance_unicast_deleted_event(inst, NULL);
		instance_state_reset(inst);
		inst->state = INST_DESTROYED;
		inst->requested_state = INST_DESTROYED;
		instance_unref(inst);
		break;
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_ACTIVATED:
		info = inst->info;
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		inst->state = INST_DESTROYED;
		inst->requested_state = INST_DESTROYED;
		instance_unref(inst);
	case INST_DESTROYED:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int instance_destroy(struct inst_info *inst)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_REQUEST_TO_REACTIVATE:
		inst->requested_state = INST_DESTROYED;
		return 0;
	case INST_INIT:
		inst->state = INST_DESTROYED;
		inst->requested_state = INST_DESTROYED;
		instance_unref(inst);
		return 0;
	case INST_DESTROYED:
		inst->requested_state = INST_DESTROYED;
		return 0;
	default:
		break;
	}

	packet = packet_create("delete", "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	inst->requested_state = INST_DESTROYED;
	inst->state = INST_REQUEST_TO_DESTROY;
	inst->changing_state = 1;
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, deactivate_cb, instance_ref(inst), 0);
}

int instance_state_reset(struct inst_info *inst)
{
	enum lb_type lb_type;
	enum pd_type pd_type;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (inst->state == INST_DESTROYED)
		return 0;

	lb_type = package_lb_type(inst->info);
	pd_type = package_pd_type(inst->info);

	if (lb_type == LB_TYPE_SCRIPT && inst->lb.canvas.script)
		script_handler_unload(inst->lb.canvas.script, 0);
	else if (lb_type == LB_TYPE_BUFFER && inst->lb.canvas.buffer)
		buffer_handler_unload(inst->lb.canvas.buffer);

	if (pd_type == PD_TYPE_SCRIPT && inst->pd.canvas.script) {
		inst->pd.is_opened_for_reactivate = script_handler_is_loaded(inst->pd.canvas.script);
		script_handler_unload(inst->pd.canvas.script, 1);
	} else if (pd_type == PD_TYPE_BUFFER && inst->pd.canvas.buffer) {
		inst->pd.is_opened_for_reactivate = buffer_handler_is_loaded(inst->pd.canvas.buffer);
		buffer_handler_unload(inst->pd.canvas.buffer);
	}

	inst->state = INST_INIT;
	inst->requested_state = INST_INIT;
	return 0;
}

int instance_reactivate(struct inst_info *inst)
{
	struct packet *packet;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_DESTROY:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_REACTIVATE:
		inst->requested_state = INST_ACTIVATED;
		return 0;
	case INST_DESTROYED:
	case INST_ACTIVATED:
		return 0;
	case INST_INIT:
	default:
		break;
	}

	packet = packet_create("renew", "sssiidssiis",
			package_name(inst->info),
			inst->id,
			inst->content,
			inst->timeout,
			!!package_lb_path(inst->info),
			inst->period,
			inst->cluster,
			inst->category,
			inst->lb.width, inst->lb.height,
			package_abi(inst->info));
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	ret = slave_activate(package_slave(inst->info));
	if (ret < 0 && ret != -EALREADY) {
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
	inst->changing_state = 1;

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, reactivate_cb, instance_ref(inst), 1);
}

int instance_activate(struct inst_info *inst)
{
	struct packet *packet;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		inst->requested_state = INST_ACTIVATED;
		return 0;
	case INST_ACTIVATED:
	case INST_DESTROYED:
		return 0;
	case INST_INIT:
	default:
		break;
	}

	packet = packet_create("new", "sssiidssis",
			package_name(inst->info),
			inst->id,
			inst->content,
			inst->timeout,
			!!package_lb_path(inst->info),
			inst->period,
			inst->cluster,
			inst->category,
			!!inst->client,
			package_abi(inst->info));
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	ret = slave_activate(package_slave(inst->info));
	if (ret < 0 && ret != -EALREADY) {
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
	inst->changing_state = 1;

	/*!
	 * \note
	 * Try to activate a slave if it is not activated
	 */
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, activate_cb, instance_ref(inst), 1);
}

void instance_lb_updated(const char *pkgname, const char *id)
{
	struct inst_info *inst;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		return;

	instance_lb_updated_by_instance(inst);
}

void instance_lb_updated_by_instance(struct inst_info *inst)
{
	struct packet *packet;
	const char *id;
	enum lb_type lb_type;
	const char *title;
	const char *content;

	if (inst->client && inst->visible != LB_SHOW) {
		DbgPrint("Livebox is hidden. ignore update event\n");
		return;
	}

	lb_type = package_lb_type(inst->info);
	if (lb_type == LB_TYPE_SCRIPT)
		id = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		id = buffer_handler_id(inst->lb.canvas.buffer);
	else
		id = "";

	if (inst->content)
		content = inst->content;
	else
		content = "";

	if (inst->title)
		title = inst->title;
	else
		title = "";

	packet = packet_create_noack("lb_updated", "sssiidss",
			package_name(inst->info), inst->id, id,
			inst->lb.width, inst->lb.height, inst->lb.priority, content, title);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)CLIENT_SEND_EVENT(inst, packet);
}

void instance_pd_updated_by_instance(struct inst_info *inst, const char *descfile)
{
	struct packet *packet;
	const char *id;

	if (inst->client && inst->visible != LB_SHOW) {
		DbgPrint("Livebox is hidden. ignore update event\n");
		return;
	}

	if (!descfile)
		descfile = inst->id;

	switch (package_pd_type(inst->info)) {
	case PD_TYPE_SCRIPT:
		id = fb_id(script_handler_fb(inst->pd.canvas.script));
		break;
	case PD_TYPE_BUFFER:
		id = buffer_handler_id(inst->pd.canvas.buffer);
		break;
	case PD_TYPE_TEXT:
	default:
		id = "";
		break;
	}

	packet = packet_create_noack("pd_updated", "ssssii",
			package_name(inst->info), inst->id, descfile, id,
			inst->pd.width, inst->pd.height);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)CLIENT_SEND_EVENT(inst, packet);
}

void instance_pd_updated(const char *pkgname, const char *id, const char *descfile)
{
	struct inst_info *inst;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		return;

	instance_pd_updated_by_instance(inst, descfile);
}

void instance_set_lb_info(struct inst_info *inst, int w, int h, double priority, const char *content, const char *title)
{
	char *_content = NULL;
	char *_title = NULL;

	if (strlen(content))
		_content = strdup(content);

	if (strlen(title))
		_title = strdup(title);

	inst->lb.width = w;
	inst->lb.height = h;

	if (_content) {
		free(inst->content);
		inst->content= _content;
	}

	if (title) {
		free(inst->title);
		inst->title = _title;
	}
		

	if (priority >= 0.0f && priority <= 1.0f)
		inst->lb.priority = priority;
}

void instance_set_pd_info(struct inst_info *inst, int w, int h)
{
	inst->pd.width = w;
	inst->pd.height = h;
}

static void pinup_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct set_pinup_cbdata *cbdata = data;
	const char *content;
	struct packet *result;
	int ret;

	if (!packet) {
		/*!
		 * \todo
		 * Send pinup failed event to client.
		 */
		ret = -EINVAL;
		goto out;
	}

	if (packet_get(packet, "is", &ret, &content) != 2) {
		/*!
		 * \todo
		 * Send pinup failed event to client
		 */
		ret = -EINVAL;
		goto out;
	}

	if (ret == 0) {
		char *new_content;

		new_content = strdup(content);
		if (!new_content) {
			ErrPrint("Heap: %s\n", strerror(errno));
			/*!
			 * \note
			 * send pinup failed event to client
			 */
			ret = -ENOMEM;
			goto out;
		}
	
		cbdata->inst->is_pinned_up = cbdata->pinup;
		free(cbdata->inst->content);

		cbdata->inst->content = new_content;
	}

out:
	/*!
	 * \node
	 * Send PINUP Result to client.
	 * Client should wait this event.
	 */
	result = packet_create_noack("pinup", "iisss", ret, cbdata->inst->is_pinned_up,
							package_name(cbdata->inst->info), cbdata->inst->id, cbdata->inst->content);
	if (result)
		(void)CLIENT_SEND_EVENT(cbdata->inst, result);
	else
		ErrPrint("Failed to build a packet for %s\n", package_name(cbdata->inst->info));

	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_set_pinup(struct inst_info *inst, int pinup)
{
	struct set_pinup_cbdata *cbdata;
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	if (!package_pinup(inst->info))
		return -EINVAL;

	if (pinup == inst->is_pinned_up)
		return -EINVAL;

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata)
		return -ENOMEM;

	cbdata->inst = instance_ref(inst);
	cbdata->pinup = pinup;

	packet = packet_create("pinup", "ssi", package_name(inst->info), inst->id, pinup);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, pinup_cb, cbdata, 0);
}

int instance_freeze_updator(struct inst_info *inst)
{
	if (!inst->update_timer) {
		DbgPrint("Update timer is not exists\n");
		return -EINVAL;
	}

	DbgPrint("Freeze the update timer\n");
	ecore_timer_freeze(inst->update_timer);
	return 0;
}

int instance_thaw_updator(struct inst_info *inst)
{
	if (!inst->update_timer) {
		DbgPrint("Update timer is not exists\n");
		return -EINVAL;
	}

	if (client_is_all_paused() || setting_is_lcd_off()) {
		DbgPrint("Skip thaw\n");
		return -EINVAL;
	}

	if (inst->visible == LB_HIDE_WITH_PAUSE) {
		DbgPrint("Live box is invisible\n");
		return -EINVAL;
	}

	DbgPrint("Thaw the update timer\n");
	ecore_timer_thaw(inst->update_timer);
	return 0;
}

enum livebox_visible_state instance_visible_state(struct inst_info *inst)
{
	return inst->visible;
}

int instance_set_visible_state(struct inst_info *inst, enum livebox_visible_state state)
{
	if (inst->visible == state) {
		DbgPrint("Visibility has no changed\n");
		return 0;
	}

	switch (state) {
	case LB_SHOW:
	case LB_HIDE:
		if (inst->visible == LB_HIDE_WITH_PAUSE) {
			if (resume_livebox(inst) == 0)
				inst->visible = state;

			instance_thaw_updator(inst);
		} else {
			inst->visible = state;
		}
		break;

	case LB_HIDE_WITH_PAUSE:
		if (pause_livebox(inst) == 0)
			inst->visible = LB_HIDE_WITH_PAUSE;

		instance_freeze_updator(inst);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void resize_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct resize_cbdata *cbdata = data;
	int ret;

	if (!packet) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	if (ret == 0) {
		cbdata->inst->lb.width = cbdata->w;
		cbdata->inst->lb.height = cbdata->h;
	} else {
		ErrPrint("Failed to change the size of a livebox (%d)\n", ret);
	}

	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_resize(struct inst_info *inst, int w, int h)
{
	struct resize_cbdata *cbdata;
	struct packet *packet;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package: %s\n", package_name(inst->info));
		return -EFAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	cbdata->inst = instance_ref(inst);
	cbdata->w = w;
	cbdata->h = h;

	/* NOTE: param is resued from here */
	packet = packet_create("resize", "ssii", package_name(inst->info), inst->id, w, h);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	ret = slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, resize_cb, cbdata, 0);
	return ret;
}

static void set_period_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;
	struct period_cbdata *cbdata = data;
	struct packet *result;

	if (!packet) {
		ret = -EFAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ret = -EINVAL;
		goto out;
	}

	if (ret == 0)
		cbdata->inst->period = cbdata->period;
	else
		ErrPrint("Failed to set period %d\n", ret);

out:
	result = packet_create_noack("period_changed", "idss", ret, cbdata->inst->period, package_name(cbdata->inst->info), cbdata->inst->id);
	if (result)
		(void)CLIENT_SEND_EVENT(cbdata->inst, result);
	else
		ErrPrint("Failed to build a packet for %s\n", package_name(cbdata->inst->info));

	instance_unref(cbdata->inst);
	free(cbdata);
	return;
}

int instance_set_period(struct inst_info *inst, double period)
{
	struct packet *packet;
	struct period_cbdata *cbdata;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (period < 0.0f) { /* Use the default period */
		period = package_period(inst->info);
	} else if (period > 0.0f && period < MINIMUM_PERIOD) {
		period = MINIMUM_PERIOD; /* defined at conf.h */
	}

	cbdata->period = period;
	cbdata->inst = instance_ref(inst);

	packet = packet_create("set_period", "ssd", package_name(inst->info), inst->id, period);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, set_period_cb, cbdata, 0);
}

int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	/* NOTE: param is resued from here */
	packet = packet_create_noack("clicked", "sssddd", package_name(inst->info), inst->id, event, timestamp, x, y);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

int instance_text_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	packet = packet_create_noack("text_signal", "ssssdddd", package_name(inst->info), inst->id, emission, source, sx, sy, ex, ey);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

static void change_group_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct change_group_cbdata *cbdata = data;
	struct packet *result;
	int ret;

	if (!packet) {
		free(cbdata->cluster);
		free(cbdata->category);
		ret = -EFAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid packet\n");
		free(cbdata->cluster);
		free(cbdata->category);
		ret = -EINVAL;
		goto out;
	}

	if (ret == 0) {
		free(cbdata->inst->cluster);
		cbdata->inst->cluster = cbdata->cluster;

		free(cbdata->inst->category);
		cbdata->inst->category = cbdata->category;
	} else {
		free(cbdata->cluster);
		free(cbdata->category);
	}

out:
	result = packet_create_noack("group_changed", "ssiss",
				package_name(cbdata->inst->info), cbdata->inst->id, ret,
				cbdata->inst->cluster, cbdata->inst->category);
	if (!result)
		ErrPrint("Failed to build a packet %s\n", package_name(cbdata->inst->info));
	else
		(void)CLIENT_SEND_EVENT(cbdata->inst, result);

	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_change_group(struct inst_info *inst, const char *cluster, const char *category)
{
	struct packet *packet;
	struct change_group_cbdata *cbdata;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	cbdata->cluster = strdup(cluster);
	if (!cbdata->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(cbdata);
		return -ENOMEM;
	}

	cbdata->category = strdup(category);
	if (!cbdata->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(cbdata->cluster);
		free(cbdata);
		return -ENOMEM;
	}

	cbdata->inst = instance_ref(inst);

	packet = packet_create("change_group","ssss", package_name(inst->info), inst->id, cluster, category);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		free(cbdata->category);
		free(cbdata->cluster);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, change_group_cb, cbdata, 0);
}

const int const instance_auto_launch(const struct inst_info *inst)
{
	return inst->lb.auto_launch;
}

const int const instance_priority(const struct inst_info *inst)
{
	return inst->lb.priority;
}

const struct client_node *const instance_client(const struct inst_info *inst)
{
	return inst->client;
}

const double const instance_period(const struct inst_info *inst)
{
	return inst->period;
}

const int const instance_lb_width(const struct inst_info *inst)
{
	return inst->lb.width;
}

const int const instance_lb_height(const struct inst_info *inst)
{
	return inst->lb.height;
}

const int const instance_pd_width(const struct inst_info *inst)
{
	return inst->pd.width;
}

const int const instance_pd_height(const struct inst_info *inst)
{
	return inst->pd.height;
}

struct pkg_info *const instance_package(const struct inst_info *inst)
{
	return inst->info;
}

struct script_info *const instance_lb_script(const struct inst_info *inst)
{
	return (package_lb_type(inst->info) == LB_TYPE_SCRIPT) ? inst->lb.canvas.script : NULL;
}

struct script_info *const instance_pd_script(const struct inst_info *inst)
{
	return (package_pd_type(inst->info) == PD_TYPE_SCRIPT) ? inst->pd.canvas.script : NULL;
}

struct buffer_info *const instance_lb_buffer(const struct inst_info *inst)
{
	return (package_lb_type(inst->info) == LB_TYPE_BUFFER) ? inst->lb.canvas.buffer : NULL;
}

struct buffer_info *const instance_pd_buffer(const struct inst_info *inst)
{
	return (package_pd_type(inst->info) == PD_TYPE_BUFFER) ? inst->pd.canvas.buffer : NULL;
}

const char *const instance_id(const struct inst_info *inst)
{
	return inst->id;
}

const char *const instance_content(const struct inst_info *inst)
{
	return inst->content;
}

const char *const instance_category(const struct inst_info *inst)
{
	return inst->category;
}

const char *const instance_cluster(const struct inst_info *inst)
{
	return inst->cluster;
}

const char * const instance_title(const struct inst_info *inst)
{
	return inst->title;
}

const double const instance_timestamp(const struct inst_info *inst)
{
	return inst->timestamp;
}

const enum instance_state const instance_state(const struct inst_info *inst)
{
	return inst->state;
}

void instance_faulted(struct inst_info *inst)
{
	switch (inst->state) {
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		instance_unicast_deleted_event(inst, NULL);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_ACTIVATED:
		/*!
		 * Send deleted event to the client
		 * If this instance has owned by a client, send deleted event to it
		 * or send deleted event to all clients which subscribe this instance's cluster & sub-cluster
		 */
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	case INST_DESTROYED:
	default:
		break;
	}
}

/*!
 * Invoked when a slave is activated
 */
int instance_recover_state(struct inst_info *inst)
{
	struct pkg_info *info;
	int ret = 0;

	if (inst->changing_state) {
		DbgPrint("Doesn't need to recover the state\n");
		return 0;
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
			info = inst->info;
			instance_state_reset(inst);
			instance_destroy(inst);
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
				instance_broadcast_deleted_event(inst);
				instance_state_reset(inst);
				instance_destroy(inst);
			} else {
				ret = 1;
			}
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst);
			ret = 1;
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
int instance_need_slave(struct inst_info *inst)
{
	int ret = 0;
	struct pkg_info *info;

	if (inst->client && client_is_faulted(inst->client)) {
		info = inst->info;

		/*!
		 * \note
		 * In this case, the client is faulted(disconnected)
		 * when the client is deactivated, its liveboxes should be removed too.
		 * So if the current inst is created by the faulted client,
		 * remove it and don't try to recover its states
		 */

		DbgPrint("CLIENT FAULT: Req. to DESTROYED (%s)\n", package_name(info));
		switch (inst->state) {
		case INST_ACTIVATED:
		case INST_REQUEST_TO_REACTIVATE:
		case INST_REQUEST_TO_DESTROY:
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		case INST_INIT:
		case INST_REQUEST_TO_ACTIVATE:
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		case INST_DESTROYED:
			break;
		}

		return 0;
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
			info = inst->info;
			instance_state_reset(inst);
			instance_destroy(inst);
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
			instance_destroy(inst);
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

int instance_slave_open_pd(struct inst_info *inst)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	const struct pkg_info *info;

	slave = package_slave(instance_package(inst));
	if (!slave)
		return -EFAULT;

	info = instance_package(inst);
	if (!info)
		return -EINVAL;

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id)
		return -EINVAL;

	packet = packet_create_noack("pd_show", "ssii", pkgname, id, instance_pd_width(inst), instance_pd_height(inst));
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return slave_rpc_request_only(slave, pkgname, packet, 0);
}

int instance_slave_close_pd(struct inst_info *inst)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	struct pkg_info *info;

	slave = package_slave(instance_package(inst));
	if (!slave)
		return -EFAULT;

	info = instance_package(inst);
	if (!info)
		return -EINVAL;

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id)
		return -EINVAL;

	packet = packet_create_noack("pd_hide", "ss", pkgname, id);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return slave_rpc_request_only(slave, pkgname, packet, 0);
}

int instance_client_pd_created(struct inst_info *inst, int status)
{
	struct packet *packet;
	const char *buf_id;

	if (inst->pd.need_to_send_close_event) {
		DbgPrint("PD is already created\n");
		return -EINVAL;
	}

	switch (package_pd_type(inst->info)) {
	case PD_TYPE_SCRIPT:
		buf_id = fb_id(script_handler_fb(inst->pd.canvas.script));
		break;
	case PD_TYPE_BUFFER:
		buf_id = buffer_handler_id(inst->pd.canvas.buffer);
		break;
	case PD_TYPE_TEXT:
	default:
		buf_id = "";
		break;
	}

	inst->pd.need_to_send_close_event = 1;

	packet = packet_create_noack("pd_created", "sssiii", 
			package_name(inst->info), inst->id, buf_id,
			inst->pd.width, inst->pd.height, status);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return CLIENT_SEND_EVENT(inst, packet);
}

int instance_client_pd_destroyed(struct inst_info *inst, int status)
{
	if (!inst->pd.need_to_send_close_event) {
		DbgPrint("PD is not created\n");
		return -EINVAL;
	}

	inst->pd.need_to_send_close_event = 0;

	return send_pd_destroyed_to_client(inst, status);
}

static int viewer_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	inst->client_list = eina_list_remove(inst->client_list, client);
	return -1; /*!< Remove this callback from the cb list */
}

int instance_add_client(struct inst_info *inst, struct client_node *client)
{
	inst->client_list = eina_list_append(inst->client_list, client);
	client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst);
	return 0;
}

int instance_del_client(struct inst_info *inst, struct client_node *client)
{
	if (!eina_list_data_find(inst->client_list, client)) {
		DbgPrint("Client(%p) is not registered as a viewer of this instance(%p)\n", client, inst);
		return -ENOENT;
	}

	client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst);
	inst->client_list = eina_list_remove(inst->client_list, client);
	return 0;
}

int instance_has_client(struct inst_info *inst, struct client_node *client)
{
	return !!eina_list_data_find(inst->client_list, client);
}

void *instance_client_list(struct inst_info *inst)
{
	return inst->client_list;
}

/* End of a file */
