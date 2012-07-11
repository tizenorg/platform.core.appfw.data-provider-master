#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <dlog.h>
#include <Ecore_Evas.h>
#include <Eina.h>
#include <gio/gio.h>

#include <packet.h>
#include <com-core_packet.h>

#include "conf.h"
#include "util.h"
#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "client_rpc.h"
#include "package.h"
#include "instance.h"
#include "fb.h"
#include "script_handler.h"
#include "buffer_handler.h"

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

	char *id;
	double timestamp;

	char *content;
	char *cluster;
	char *category;

	struct {
		int width;
		int height;
		double priority;
		int is_pinned_up;

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
	} pd;

	int timeout;
	double period;

	struct client_node *client;
	int refcnt;
};

int instance_unicast_created_event(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;
	enum lb_type lb_type;
	enum pd_type pd_type;
	const char *lb_file;
	const char *pd_file;

	if (!inst->client && !client)
		return 0;

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

	packet = packet_create("created", "dsssiiiissssidiiiiid", 
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
			inst->period);
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	if (!client)
		client = inst->client;

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

	packet = packet_create("created", "dsssiiiissssidiiiiid", 
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
			inst->period);

	if (!packet)
		return -EFAULT;

	return client_rpc_broadcast(packet);
}

int instance_unicast_deleted_event(struct inst_info *inst)
{
	struct packet *packet;

	if (!inst->client)
		return -EINVAL;

	packet = packet_create("deleted", "ssd", package_name(inst->info), inst->id, inst->timestamp);
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}
		
	return client_rpc_async_request(inst->client, packet);
}

int instance_broadcast_deleted_event(struct inst_info *inst)
{
	struct packet *packet;

	packet = packet_create("deleted", "ssd", package_name(inst->info), inst->id, inst->timestamp);
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}
		
	return client_rpc_broadcast(packet);
}

static int client_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	instance_destroy(inst);
	return 0;
}

static inline void destroy_instance(struct inst_info *inst)
{
	struct pkg_info *pkg;
	enum lb_type lb_type;
	enum pd_type pd_type;

	pkg = inst->info;

	lb_type = package_lb_type(pkg);
	pd_type = package_pd_type(pkg);

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

	free(inst->category);
	free(inst->cluster);
	free(inst->content);
	util_unlink(inst->id);
	free(inst->id);
	package_del_instance(inst->info, inst);
	free(inst);
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

	snprintf(id, sizeof(id), "file://%s%s_%d_%lf.png", g_conf.path.image, pkgname, client_pid(client), inst->timestamp);
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

	if (fork_package(inst, pkgname) < 0) {
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
	package_add_instance(inst->info, inst);
	instance_activate(inst);
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

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * The instance_reload will care this.
		 * And it will be called from the slave activate callback.
		 */
		instance_unref(inst);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid argument\n");
		instance_unref(inst);
		return;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
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
			DbgPrint("==\n");
			info = inst->info;
			instance_broadcast_deleted_event(inst);
			instance_state_reset(inst);
			instance_destroy(inst);
			slave_unload_instance(package_slave(info));
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
		slave_unload_instance(package_slave(info));
		break;
	}

	instance_unref(inst);
}

static void reactivate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	struct pkg_info *info;
	enum lb_type lb_type;
	enum pd_type pd_type;
	int ret;

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * instance_reload function will care this.
		 * and it will be called from the slave_activate callback
		 */
		instance_unref(inst);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		instance_unref(inst);
		return;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		instance_unref(inst);
		return;
	}

	switch (ret) {
	case 0: /*!< normally created */
		inst->state = INST_ACTIVATED;
		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_destroy(inst);
			break;
		case INST_ACTIVATED:
			info = inst->info;
			lb_type = package_lb_type(info);
			pd_type = package_pd_type(info);

			if (lb_type == LB_TYPE_SCRIPT && inst->lb.canvas.script)
				script_handler_load(inst->lb.canvas.script, 0);
			else if (lb_type == LB_TYPE_BUFFER && inst->lb.canvas.buffer)
				buffer_handler_load(inst->lb.canvas.buffer);

			if (pd_type == PD_TYPE_SCRIPT && inst->pd.canvas.script && inst->pd.is_opened_for_reactivate)
				script_handler_load(inst->pd.canvas.script, 1);
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
		slave_unload_instance(package_slave(info));
		break;
	}

	instance_unref(inst);
}

static void activate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	struct inst_info *new_inst;
	int ret;
	int w;
	int h;
	double priority;

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * instance_reload will care this
		 * it will be called from the slave_activate callback
		 */
		instance_unref(inst);
		return;
	}

	if (packet_get(packet, "iiid", &ret, &w, &h, &priority) != 4) {
		ErrPrint("Invalid parameter\n");
		instance_unref(inst);
		return;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		instance_unref(inst);
		return;
	}

	switch (ret) {
	case 1: /*!< need to create */
		new_inst = instance_create(inst->client, util_timestamp(), package_name(inst->info),
						inst->content, inst->cluster, inst->category,
						inst->period);
	case 0: /*!< normally created */
		/*!
		 * \note
		 * Anyway this instance is loaded to the slave,
		 * so just increase the loaded instance counter
		 * After that, do reset jobs.
		 */
		inst->state = INST_ACTIVATED;

		inst->lb.width = w;
		inst->lb.height = h;
		inst->lb.priority = priority;

		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_unicast_deleted_event(inst);
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		case INST_ACTIVATED:
		default:
			/*!
			 * \note
			 * LB should be created at the create time
			 */
			if (package_lb_type(inst->info) == LB_TYPE_SCRIPT) {
				inst->lb.canvas.script = script_handler_create(inst,
								package_lb_path(inst->info), package_lb_group(inst->info),
								inst->lb.width, inst->lb.height);

				if (!inst->lb.canvas.script)
					ErrPrint("Failed to create LB\n");
				else
					script_handler_load(inst->lb.canvas.script, 0);
			} else if (package_lb_type(inst->info) == LB_TYPE_BUFFER) {
				inst->lb.canvas.buffer = buffer_handler_create(inst, BUFFER_TYPE_FILE, inst->lb.width, inst->lb.height, sizeof(int));
				if (!inst->lb.canvas.buffer)
					ErrPrint("Failed to create LB\n");
			}

			if (package_pd_type(inst->info) == PD_TYPE_SCRIPT) {
				if (inst->pd.width == 0 && inst->pd.height == 0) {
					inst->pd.width = package_pd_width(inst->info);
					inst->pd.height = package_pd_height(inst->info);
				}

				inst->pd.canvas.script = script_handler_create(inst,
								package_pd_path(inst->info), package_pd_group(inst->info),
								inst->pd.width, inst->pd.height);
				if (!inst->pd.canvas.script)
					ErrPrint("Failed to create PD\n");
			} else if (package_pd_type(inst->info) == PD_TYPE_BUFFER) {
				if (inst->pd.width == 0 && inst->pd.height == 0) {
					inst->pd.width = package_pd_width(inst->info);
					inst->pd.height = package_pd_height(inst->info);
				}

				inst->pd.canvas.buffer = buffer_handler_create(inst, BUFFER_TYPE_FILE, inst->pd.width, inst->pd.height, sizeof(int));
				if (!inst->pd.canvas.buffer)
					ErrPrint("Failed to create PD\n");
			}

			slave_load_instance(package_slave(inst->info));
			instance_broadcast_created_event(inst);
			break;
		}
		break;
	default:
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(inst->info), ret);
		instance_unicast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

	instance_unref(inst);
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
		instance_unicast_deleted_event(inst);
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
		slave_unload_instance(package_slave(info));
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
		ErrPrint("Failed to build a delete param\n");
		return -EFAULT;
	}

	inst->requested_state = INST_DESTROYED;
	inst->state = INST_REQUEST_TO_DESTROY;
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, deactivate_cb, instance_ref(inst));
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

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Unable to reactivate the instance of a fault package\n");
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

	packet = packet_create("renew", "sssiidssiiis",
			package_name(inst->info),
			inst->id,
			inst->content,
			inst->timeout,
			!!package_lb_path(inst->info),
			inst->period,
			inst->cluster,
			inst->category,
			inst->lb.is_pinned_up,
			inst->lb.width, inst->lb.height,
			package_abi(inst->info));
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	inst->requested_state = INST_ACTIVATED;
	inst->state = INST_REQUEST_TO_REACTIVATE;

	slave_activate(package_slave(inst->info));
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, reactivate_cb, instance_ref(inst));
}

int instance_activate(struct inst_info *inst)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Unable to activate the instance of a fault package\n");
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

	packet = packet_create("new", "sssiidssiis",
			package_name(inst->info),
			inst->id,
			inst->content,
			inst->timeout,
			!!package_lb_path(inst->info),
			inst->period,
			inst->cluster,
			inst->category,
			inst->lb.is_pinned_up,
			!!inst->client,
			package_abi(inst->info));
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	inst->state = INST_REQUEST_TO_ACTIVATE;
	inst->requested_state = INST_ACTIVATED;

	/*!
	 * \note
	 * Try to activate a slave if it is not activated
	 */
	slave_activate(package_slave(inst->info));
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, activate_cb, instance_ref(inst));
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

	lb_type = package_lb_type(inst->info);
	if (lb_type == LB_TYPE_SCRIPT)
		id = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		id = buffer_handler_id(inst->lb.canvas.buffer);
	else
		id = "";

	packet = packet_create("lb_updated", "sssiid",
			package_name(inst->info), inst->id, id,
			inst->lb.width, inst->lb.height, inst->lb.priority);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	client_rpc_broadcast(packet);
}

void instance_pd_updated_by_instance(struct inst_info *inst, const char *descfile)
{
	struct packet *packet;
	enum pd_type pd_type;
	const char *id;

	if (!descfile)
		descfile = inst->id;

	pd_type = package_pd_type(inst->info);
	if (pd_type == PD_TYPE_SCRIPT)
		id = fb_id(script_handler_fb(inst->pd.canvas.script));
	else if (pd_type == PD_TYPE_BUFFER)
		id = buffer_handler_id(inst->pd.canvas.buffer);
	else
		id = "";

	packet = packet_create("pd_updated", "ssssii",
			package_name(inst->info), inst->id, descfile, id,
			inst->pd.width, inst->pd.height);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	client_rpc_broadcast(packet);
}

void instance_pd_updated(const char *pkgname, const char *id, const char *descfile)
{
	struct inst_info *inst;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		return;

	instance_pd_updated_by_instance(inst, descfile);
}

void instance_set_lb_info(struct inst_info *inst, int w, int h, double priority)
{
	inst->lb.width = w;
	inst->lb.height = h;

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
	int ret;

	if (!packet)
		goto out;

	if (packet_get(packet, "i", &ret) != 1)
		goto out;

	if (ret == 0)
		cbdata->inst->lb.is_pinned_up = cbdata->pinup;

out:
	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_set_pinup(struct inst_info *inst, int pinup)
{
	struct set_pinup_cbdata *cbdata;
	struct packet *packet;

	if (package_is_fault(inst->info))
		return -EFAULT;

	if (!package_pinup(inst->info))
		return -EINVAL;

	if (pinup == inst->lb.is_pinned_up)
		return 0;

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata)
		return -ENOMEM;

	cbdata->inst = instance_ref(inst);
	cbdata->pinup = pinup;

	packet = packet_create("pinup", "ssi", package_name(inst->info), inst->id, pinup);
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, pinup_cb, cbdata);
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
		DbgPrint("Failed to build a packet for %s\n", package_name(inst->info));
		ret = -EFAULT;
		instance_unref(cbdata->inst);
		free(cbdata);
	} else {
		ret = slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, resize_cb, cbdata);
	}

	return ret;
}

static void set_period_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;
	struct period_cbdata *cbdata = data;

	if (!packet) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	if (ret == 0)
		cbdata->inst->period = cbdata->period;
	else
		ErrPrint("Failed to set period %d\n", ret);

	instance_unref(cbdata->inst);
	free(cbdata);
	return;
}

int instance_set_period(struct inst_info *inst, double period)
{
	struct packet *packet;
	struct period_cbdata *cbdata;

	if (package_is_fault(inst->info))
		return -EFAULT;

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
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, set_period_cb, cbdata);
}

int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y)
{
	struct packet *packet;

	if (package_is_fault(inst->info))
		return -EFAULT;

	/* NOTE: param is resued from here */
	packet = packet_create("clicked", "sssddd", package_name(inst->info), inst->id, event, timestamp, x, y);
	if (!packet)
		return -EFAULT;

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, NULL, NULL);
}

int instance_text_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey)
{
	struct packet *packet;

	if (package_is_fault(inst->info))
		return -EFAULT;

	packet = packet_create("text_signal", "ssssdddd", package_name(inst->info), inst->id, emission, source, sx, sy, ex, ey);
	if (!packet)
		return -EFAULT;

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, NULL, NULL);
}

static void change_group_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct change_group_cbdata *cbdata = data;
	int ret;

	if (!packet) {
		instance_unref(cbdata->inst);
		free(cbdata->cluster);
		free(cbdata->category);
		free(cbdata);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid packet\n");
		instance_unref(cbdata->inst);
		free(cbdata->cluster);
		free(cbdata->category);
		free(cbdata);
		return;
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

	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_change_group(struct inst_info *inst, const char *cluster, const char *category)
{
	struct packet *packet;
	struct change_group_cbdata *cbdata;

	if (package_is_fault(inst->info))
		return -EFAULT;

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
		instance_unref(cbdata->inst);
		free(cbdata->category);
		free(cbdata->cluster);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, change_group_cb, cbdata);
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

const struct pkg_info *const instance_package(const struct inst_info *inst)
{
	return inst->info;
}

struct script_info *const instance_lb_script(const struct inst_info *inst)
{
	return (package_lb_type(inst->info) == LB_TYPE_SCRIPT) ? inst->lb.canvas.script : NULL;
}

struct script_info * const instance_pd_script(const struct inst_info *inst)
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
	struct pkg_info *info;
	DbgPrint("Fault. DESTROYING (%s)\n", package_name(inst->info));

	switch (inst->state) {
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		instance_unicast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_ACTIVATED:
		DbgPrint("==\n");
		info = inst->info;
		instance_state_reset(inst);
		instance_broadcast_deleted_event(inst);
		instance_destroy(inst);
		slave_unload_instance(package_slave(info));
		break;
	case INST_DESTROYED:
	default:
		break;
	}
}

/*!
 * Invoked when a slave is activated
 */
void instance_recover_state(struct inst_info *inst)
{
	struct pkg_info *info;
	switch (inst->state) {
	case INST_ACTIVATED:
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			DbgPrint("Req. to RE-ACTIVATED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_reactivate(inst);
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			info = inst->info;
			instance_state_reset(inst);
			instance_destroy(inst);
			slave_unload_instance(package_slave(info));
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
			instance_activate(inst);
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
			slave_unload_instance(package_slave(info));
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
			slave_unload_instance(package_slave(info));
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

/* End of a file */
