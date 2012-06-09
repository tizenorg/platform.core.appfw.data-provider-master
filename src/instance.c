#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>

#include <dlog.h>
#include <Ecore_Evas.h>
#include <Eina.h>
#include <gio/gio.h>

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
	enum instance_state requested_state; /*!< Only ACTIVATED | DEACTIVATED is acceptable */

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
		struct script_info *handle;

		int auto_launch;
	} lb;

	struct {
		int width;
		int height;
		struct script_info *handle;
	} pd;

	int timeout;
	double period;

	struct client_node *client;
	int refcnt;
};

int instance_unicast_created_event(struct inst_info *inst, struct client_node *client)
{
	GVariant *param;

	if (!inst->client && !client) {
		DbgPrint("Instance[%s] is created by system\n", package_name(inst->info));
		return 0;
	}

	param = g_variant_new("(dsssiiiissssidiiiiid)", 
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->lb.width, inst->lb.height,
			inst->pd.width, inst->pd.height,
			inst->cluster, inst->category,
			fb_filename(script_handler_fb(inst->lb.handle)),
			fb_filename(script_handler_fb(inst->pd.handle)),
			inst->lb.auto_launch,
			inst->lb.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			package_lb_type(inst->info) == LB_TYPE_TEXT,
			package_pd_type(inst->info) == PD_TYPE_TEXT,
			inst->period);

	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	if (!client)
		client = inst->client;

	return client_rpc_async_request(client, "created", param);
}

int instance_broadcast_created_event(struct inst_info *inst)
{
	GVariant *param;

	param = g_variant_new("(dsssiiiissssidiiiiid)", 
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->lb.width, inst->lb.height,
			inst->pd.width, inst->pd.height,
			inst->cluster, inst->category,
			fb_filename(script_handler_fb(inst->lb.handle)),
			fb_filename(script_handler_fb(inst->pd.handle)),
			inst->lb.auto_launch,
			inst->lb.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			package_lb_type(inst->info) == LB_TYPE_TEXT,
			package_pd_type(inst->info) == PD_TYPE_TEXT,
			inst->period);

	if (!param)
		return -EFAULT;

	return client_rpc_broadcast("created", param);
}

int instance_unicast_deleted_event(struct inst_info *inst)
{
	GVariant *param;

	if (!inst->client)
		return -EINVAL;

	param = g_variant_new("(ssd)", package_name(inst->info), inst->id, inst->timestamp);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}
		
	return client_rpc_async_request(inst->client, "deleted", param);
}

int instance_broadcast_deleted_event(struct inst_info *inst)
{
	GVariant *param;

	param = g_variant_new("(ssd)", package_name(inst->info), inst->id, inst->timestamp);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}
		
	return client_rpc_broadcast("deleted", param);
}

static int client_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;

	DbgPrint("Instance destroying: %s\n", package_name(instance_package(inst)));
	switch (inst->state) {
	case INST_ACTIVATED:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_DEACTIVATED:
		DbgPrint("Destroy\n");
		instance_destroy(inst);
		break;
	case INST_REQUEST_TO_DEACTIVATE:
		DbgPrint("Requested state is changed to INST_DESTROY\n");
		inst->requested_state = INST_DESTROY;
		break;
	default:
		break;
	}
	return 0;
}

static inline void destroy_instance(struct inst_info *inst)
{
	DbgPrint("Destroy instance for %s\n", package_name(inst->info));

	if (inst->lb.handle) {
		script_handler_unload(inst->lb.handle, 0);
		script_handler_destroy(inst->lb.handle);
	}

	if (inst->pd.handle) {
		script_handler_unload(inst->pd.handle, 1);
		script_handler_destroy(inst->pd.handle);
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

	snprintf(id, sizeof(id), "%s%s_%d_%lf.png", g_conf.path.image, pkgname, client_pid(client), inst->timestamp);
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

	inst->state = INST_DEACTIVATED;
	instance_ref(inst);

	package_add_instance(inst->info, inst);

	DbgPrint("Create a new instance: id[%s], pkg[%s], content[%s], cluster[%s], category[%s], period[%lf]\n",
								id, pkgname, content, cluster, category, period);

	return inst;
}

int instance_destroyed(struct inst_info *inst)
{
	switch (inst->state) {
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DEACTIVATE:
		inst->requested_state = INST_DESTROYED;
		return 0;
	case INST_ACTIVATED:
		DbgPrint("Call unload instance (%s)\n", package_name(inst->info));
		slave_unload_instance(package_slave(inst->info));
		DbgPrint("Broadcast deleted event\n");
		instance_broadcast_deleted_event(inst);
	case INST_DEACTIVATED:
		inst->requested_state = INST_DESTROYED;
		inst->state = INST_DESTROYED;
		instance_destroy(inst);
		break;
	case INST_DESTROYED:
	case INST_DESTROY:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int instance_destroy(struct inst_info *inst)
{
	if (inst->state == INST_DEACTIVATED || inst->state == INST_DESTROYED) {
		instance_unref(inst);
		return 0;
	}

	inst->requested_state = INST_DESTROY;
	instance_deactivate(inst);
	return 0;
}

struct inst_info * instance_ref(struct inst_info *inst)
{
	if (!inst)
		return NULL;

	inst->refcnt++;
	return inst;
}

struct inst_info * instance_unref(struct inst_info *inst)
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

struct inst_info *instance_find_by_id(const char *pkgname, const char *id)
{
	Eina_List *l;
	Eina_List *list;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("Package %s is not exists\n", pkgname);
		return NULL;
	}

	list = package_instance_list(info);

	EINA_LIST_FOREACH(list, l, inst) {
		if (!strcmp(inst->id, id))
			return inst;
	}

	return NULL;
}

struct inst_info *instance_find_by_timestamp(const char *pkgname, double timestamp)
{
	Eina_List *l;
	Eina_List *list;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("Package %s is not exists\n", pkgname);
		return NULL;
	}

	list = package_instance_list(info);

	EINA_LIST_FOREACH(list, l, inst) {
		if (inst->timestamp == timestamp)
			return inst;
	}

	return NULL;
}

static void deactivate_cb(struct slave_node *slave, const char *funcname, GVariant *result, void *data)
{
	struct inst_info *inst = data;
	int ret;

	if (!result) {
		ErrPrint("Failed to deactivate an instance: %s\n", inst->id);
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			inst->state = INST_ACTIVATED; /*!< To reactivate this at the slave_deactivate_cb */
			break;
		case INST_DEACTIVATED:
			DbgPrint("Call unload instance (%s)\n", package_name(inst->info));
			slave_unload_instance(package_slave(inst->info));
		case INST_DESTROY:
		case INST_DESTROYED:
			instance_broadcast_deleted_event(inst);
			inst->state = INST_DESTROYED;
			instance_destroy(inst);
		default:
			break;
		}

		instance_unref(inst);
		return;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);

	switch (ret) {
	case -EINVAL:
		/*!
		 * Slave has no instance of this package.
		 */
	case -ENOENT:
		/*!
		 * Slave has no instance of this.
		 */
		slave_unload_instance(package_slave(inst->info));
		instance_broadcast_deleted_event(inst);
		if (inst->lb.handle) {
			script_handler_unload(inst->lb.handle, 0);
			script_handler_destroy(inst->lb.handle);
			inst->lb.handle = NULL;
		}

		if (inst->pd.handle) {
			script_handler_unload(inst->pd.handle, 1);
			script_handler_destroy(inst->pd.handle);
			inst->pd.handle = NULL;
		}
		/*!
		 * \note
		 * In this case, ignore the requested_state
		 * Because, this instance is already met the problem.
		 */
		inst->state = INST_DESTROYED;
		instance_destroy(inst);
		break;
	case 0:
		/*!
		 * \note
		 * Successfully unloaded
		 */
		inst->state = INST_DEACTIVATED;

		switch (inst->requested_state) {
		case INST_ACTIVATED:
			inst->state = INST_ACTIVATED;
			instance_reactivate(inst);
			break;
		case INST_DESTROYED:
		case INST_DESTROY:
			inst->state = INST_DESTROYED;
			instance_destroy(inst);
		case INST_DEACTIVATED:
			slave_unload_instance(package_slave(inst->info));
			instance_broadcast_deleted_event(inst);
			if (inst->lb.handle) {
				script_handler_unload(inst->lb.handle, 0);
				script_handler_destroy(inst->lb.handle);
				inst->lb.handle = NULL;
			}

			if (inst->pd.handle) {
				script_handler_unload(inst->pd.handle, 1);
				script_handler_destroy(inst->pd.handle);
				inst->pd.handle = NULL;
			}
		default:
			/*!< Unable to reach here */
			break;
		}

		break;
	default:
		/*!
		 * \note
		 * Failed to unload this instance.
		 * This is not possible, slave will always return -ENOENT, -EINVAL, or 0.
		 * but care this exceptional case.
		 */
		ErrPrint("Destroy function returns invalid value: %d\n", ret);
		slave_unload_instance(package_slave(inst->info));
		instance_broadcast_deleted_event(inst);
		inst->state = INST_DESTROYED;
		instance_destroy(inst);
		break;
	}

	instance_unref(inst);
}

static void reactivate_cb(struct slave_node *slave, const char *funcname, GVariant *result, void *data)
{
	struct inst_info *inst = data;
	int ret;

	if (!result) {
		ErrPrint("Failed to activate an instance: %s\n", inst->id);
		switch (inst->requested_state) {
		case INST_ACTIVATED:
		case INST_DEACTIVATED:
		case INST_DESTROY:
			slave_unload_instance(package_slave(inst->info));
		case INST_DESTROYED:
			instance_broadcast_deleted_event(inst);
			inst->state = INST_DESTROYED;
			instance_destroy(inst);
		default:
			instance_unref(inst);
			break;
		}
		return;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);

	switch (ret) {
	case 0: /*!< normally created */
		inst->state = INST_ACTIVATED;

		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_broadcast_deleted_event(inst);
			inst->state = INST_DESTROYED;
			instance_destroy(inst);
			break;
		case INST_DEACTIVATED:
		case INST_DESTROY:
			instance_deactivate(inst);
			break;
		case INST_ACTIVATED:
		default:
			break;
		}
		break;
	default:
		DbgPrint("Failed to activate an instance: %d\n", ret);
		slave_unload_instance(package_slave(inst->info));
		instance_broadcast_deleted_event(inst);
		inst->state = INST_DESTROYED;
		instance_destroy(inst);
		break;
	}

	instance_unref(inst);
}

static void activate_cb(struct slave_node *slave, const char *funcname, GVariant *result, void *data)
{
	struct inst_info *inst = data;
	struct inst_info *new_inst;
	int ret;
	int w;
	int h;
	double priority;

	if (!result) {
		ErrPrint("Failed to activate an instance: %s\n", inst->id);
		instance_unicast_deleted_event(inst);
		inst->state = INST_DESTROYED;
		instance_destroy(inst);
		instance_unref(inst);
		return;
	}

	g_variant_get(result, "(iiid)", &ret, &w, &h, &priority);
	g_variant_unref(result);

	switch (ret) {
	case 1: /*!< need to create */
		new_inst = instance_create(inst->client, util_timestamp(), package_name(inst->info),
						inst->content, inst->cluster, inst->category,
						inst->period);
		(void)instance_activate(new_inst);
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
			inst->state = INST_DESTROYED;
			instance_destroy(inst);
			break;
		case INST_DEACTIVATED:
		case INST_DESTROY:
			instance_deactivate(inst);
			/*!
			 * \note
			 * Even this instance will be destroyed, 
			 * create instance.
			 */
		case INST_ACTIVATED:
		default:
			/*!
			 * \note
			 * LB should be created at the create time
			 */
			if (package_lb_type(inst->info) == LB_TYPE_SCRIPT) {
				inst->lb.handle = script_handler_create(inst,
								package_lb_path(inst->info), package_lb_group(inst->info),
								inst->lb.width, inst->lb.height);

				if (!inst->lb.handle)
					ErrPrint("Failed to create LB\n");
				else
					script_handler_load(inst->lb.handle, 0);
			}

			if (package_pd_type(inst->info) == PD_TYPE_SCRIPT) {
				inst->pd.handle = script_handler_create(inst,
								package_pd_path(inst->info), package_pd_group(inst->info),
								inst->pd.width, inst->pd.height);
				if (!inst->pd.handle)
					ErrPrint("Failed to create PD\n");
			}

			slave_load_instance(package_slave(inst->info));
			instance_broadcast_created_event(inst);
			break;
		}
		break;
	default:
		DbgPrint("Failed to activate an instance: %d\n", ret);
		instance_unicast_deleted_event(inst);
		inst->state = INST_DESTROYED;
		instance_destroy(inst);
		break;
	}

	instance_unref(inst);
}

int instance_deactivated(struct inst_info *inst)
{
	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (inst->state == INST_ACTIVATED) {
		DbgPrint("Call unload instance (%s)\n", package_name(inst->info));
		slave_unload_instance(package_slave(inst->info));
	}

	if (inst->lb.handle) {
		script_handler_unload(inst->lb.handle, 0);
		script_handler_destroy(inst->lb.handle);
		inst->lb.handle = NULL;
	}

	if (inst->pd.handle) {
		script_handler_unload(inst->pd.handle, 1);
		script_handler_destroy(inst->pd.handle);
		inst->pd.handle = NULL;
	}

	inst->state = INST_DEACTIVATED;
	inst->requested_state = INST_DEACTIVATED;
	return 0;
}

int instance_deactivate(struct inst_info *inst)
{
	GVariant *param;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (inst->state == INST_DEACTIVATED || inst->state == INST_REQUEST_TO_DEACTIVATE) {
		/*!< Don't overwrite the requested state if it is INST_DESTROY. */
		if (inst->requested_state == INST_DESTROY)
			return 0;

		inst->requested_state = INST_DEACTIVATED;
		return 0;
	}

	if (inst->state == INST_REQUEST_TO_ACTIVATE) {
		/*!< Don't overwrite the requested state if it is INST_DESTROY. */
		if (inst->requested_state == INST_DESTROY)
			return 0;

		inst->requested_state = INST_DEACTIVATED;
		return 0;
	}

	param = g_variant_new("(ss)", package_name(inst->info), inst->id);
	if (!param)
		return -EFAULT;

	inst->state = INST_REQUEST_TO_DEACTIVATE;
	/*!< Don't overwrite the requested state if it is INST_DESTROY. */
	if (inst->requested_state != INST_DESTROY)
		inst->requested_state = INST_DEACTIVATED;

	return slave_rpc_async_request(package_slave(inst->info),
			package_name(inst->info), inst->id,
			"delete", param,
			deactivate_cb, instance_ref(inst));
}

int instance_reactivate(struct inst_info *inst)
{
	GVariant *param;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (inst->state != INST_ACTIVATED) {
		ErrPrint("Re-activation is only for activated instance\n");
		return -EINVAL;
	}

	param = g_variant_new("(sssiidssiiis)",
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
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	inst->state = INST_REQUEST_TO_ACTIVATE;
	inst->requested_state = INST_ACTIVATED;

	slave_activate(package_slave(inst->info));
	DbgPrint("Reactivate: %s\n", package_name(inst->info));
	return slave_rpc_async_request(package_slave(inst->info),
			package_name(inst->info), inst->id,
			"renew", param,
			reactivate_cb, instance_ref(inst));
}

int instance_activate(struct inst_info *inst)
{
	GVariant *param;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (inst->state == INST_ACTIVATED || inst->state == INST_REQUEST_TO_ACTIVATE) {
		/*!
		 * \note
		 * overwrite the requested state.
		 */
		inst->requested_state = INST_ACTIVATED;
		return 0;
	}

	if (inst->state == INST_REQUEST_TO_DEACTIVATE) {
		/*!
		 * \note
		 * ok, let's recreate this from delete return callback.
		 */
		inst->requested_state = INST_ACTIVATED;
		return 0;
	}

	param = g_variant_new("(sssiidssiis)",
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
	if (!param) {
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
	return slave_rpc_async_request(package_slave(inst->info),
				package_name(inst->info), inst->id,
				"new", param,
				activate_cb, instance_ref(inst));
}

void instance_lb_updated(const char *pkgname, const char *id)
{
	struct inst_info *inst;

	inst = instance_find_by_id(pkgname, id);
	if (!inst)
		return;

	instance_lb_updated_by_instance(inst);
}

void instance_lb_updated_by_instance(struct inst_info *inst)
{
	GVariant *param;

	param = g_variant_new("(ssiid)", package_name(inst->info), inst->id,
				inst->lb.width, inst->lb.height, inst->lb.priority);
	if (!param) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	client_rpc_broadcast("lb_updated", param);
}

void instance_pd_updated_by_instance(struct inst_info *inst, const char *descfile)
{
	GVariant *param;

	if (!descfile)
		descfile = inst->id;

	param = g_variant_new("(sssii)", package_name(inst->info), inst->id, descfile,
						inst->pd.width, inst->pd.height);
	if (!param) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	client_rpc_broadcast("pd_updated", param);
}

void instance_pd_updated(const char *pkgname, const char *id, const char *descfile)
{
	struct inst_info *inst;

	inst = instance_find_by_id(pkgname, id);
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

static void pinup_cb(struct slave_node *slave, const char *funcnane, GVariant *result, void *data)
{
	struct set_pinup_cbdata *cbdata = data;
	int ret;

	if (!result) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);

	if (ret == 0)
		cbdata->inst->lb.is_pinned_up = cbdata->pinup;

	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_set_pinup(struct inst_info *inst, int pinup)
{
	struct set_pinup_cbdata *cbdata;
	GVariant *param;

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

	param = g_variant_new("(ssi)", package_name(inst->info), inst->id, pinup);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info),
					package_name(inst->info), inst->id,
					"pinup", param,
					pinup_cb, cbdata);
}

static void resize_cb(struct slave_node *slave, const char *funcname, GVariant *result, void *data)
{
	struct resize_cbdata *cbdata = data;
	int ret;

	if (!result) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);

	if (ret == 0) {
		cbdata->inst->lb.width = cbdata->w;
		cbdata->inst->lb.height = cbdata->h;
	} else {
		ErrPrint("Failed to change the size of a livebox\n");
	}

	instance_unref(cbdata->inst);
	free(cbdata);
}

int instance_resize(struct inst_info *inst, int w, int h)
{
	struct resize_cbdata *cbdata;
	GVariant *param;
	int ret;

	if (package_is_fault(inst->info))
		return -EFAULT;

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	cbdata->inst = instance_ref(inst);
	cbdata->w = w;
	cbdata->h = h;

	/* NOTE: param is resued from here */
	param = g_variant_new("(ssii)", package_name(inst->info), inst->id, w, h);
	if (!param) {
		ret = -EFAULT;
		instance_unref(cbdata->inst);
		free(cbdata);
	} else {
		ret = slave_rpc_async_request(
				package_slave(inst->info),
				package_name(inst->info), inst->id,
				"resize", param,
				resize_cb, cbdata);
	}

	return ret;
}

static void set_period_cb(struct slave_node *slave, const char *funcname, GVariant *result, void *data)
{
	int ret;
	struct period_cbdata *cbdata = data;

	if (!result) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);

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
	GVariant *param;
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

	param = g_variant_new("(ssd)", package_name(inst->info), inst->id, period);
	if (!param) {
		instance_unref(cbdata->inst);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info),
					package_name(inst->info), inst->id,
					"set_period", param,
					set_period_cb, cbdata);
}

int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y)
{
	GVariant *param;

	if (package_is_fault(inst->info))
		return -EFAULT;

	/* NOTE: param is resued from here */
	param = g_variant_new("(sssddd)", package_name(inst->info), inst->id, event, timestamp, x, y);
	if (!param)
		return -EFAULT;

	return slave_rpc_async_request(package_slave(inst->info),
					package_name(inst->info), inst->id,
					"clicked", param,
					NULL, NULL);
}

int instance_text_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey)
{
	GVariant *param;

	if (package_is_fault(inst->info))
		return -EFAULT;

	param = g_variant_new("(ssssdddd)", package_name(inst->info), inst->id, emission, source, sx, sy, ex, ey);
	if (!param)
		return -EFAULT;

	return slave_rpc_async_request(package_slave(inst->info),
				package_name(inst->info), inst->id,
				"text_signal", param,
				NULL, NULL);
}

static void change_group_cb(struct slave_node *slave, const char *funcname, GVariant *result, void *data)
{
	struct change_group_cbdata *cbdata = data;
	int ret;

	if (!result) {
		instance_unref(cbdata->inst);
		free(cbdata->cluster);
		free(cbdata->category);
		free(cbdata);
		return;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);

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
	GVariant *param;
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

	param = g_variant_new("(ssss)", package_name(inst->info), inst->id, cluster, category);
	if (!param) {
		instance_unref(cbdata->inst);
		free(cbdata->category);
		free(cbdata->cluster);
		free(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info),
					package_name(inst->info), inst->id,
					"change_group", param,
					change_group_cb, cbdata);
}

int const instance_auto_launch(struct inst_info *inst)
{
	return inst->lb.auto_launch;
}

int const instance_priority(struct inst_info *inst)
{
	return inst->lb.priority;
}

struct client_node *const instance_client(struct inst_info *inst)
{
	return inst->client;
}

double const instance_period(struct inst_info *inst)
{
	return inst->period;
}

int const instance_lb_width(struct inst_info *inst)
{
	return inst->lb.width;
}

int const instance_lb_height(struct inst_info *inst)
{
	return inst->lb.height;
}

int const instance_pd_width(struct inst_info *inst)
{
	return inst->pd.width;
}

int const instance_pd_height(struct inst_info *inst)
{
	return inst->pd.height;
}

struct pkg_info *const instance_package(struct inst_info *inst)
{
	return inst->info;
}

struct script_info *const instance_lb_handle(struct inst_info *inst)
{
	return inst->lb.handle;
}

struct script_info * const instance_pd_handle(struct inst_info *inst)
{
	return inst->pd.handle;
}

char *const instance_id(struct inst_info *inst)
{
	return inst->id;
}

char *const instance_content(struct inst_info *inst)
{
	return inst->content;
}

char *const instance_category(struct inst_info *inst)
{
	return inst->category;
}

char *const instance_cluster(struct inst_info *inst)
{
	return inst->cluster;
}

double const instance_timestamp(struct inst_info *inst)
{
	return inst->timestamp;
}

void instance_set_state(struct inst_info *inst, enum instance_state state)
{
	inst->state = state;
}

enum instance_state const instance_state(struct inst_info *inst)
{
	return inst->state;
}

/* End of a file */
