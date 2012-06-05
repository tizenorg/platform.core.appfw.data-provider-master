#include <stdio.h>
#include <errno.h>
#include <string.h> /* strcmp */
#include <stdlib.h> /* free */
#include <libgen.h> /* basename */

#include <Ecore_Evas.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <dlog.h>

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_manager.h"
#include "group.h"
#include "util.h"
#include "parser.h"
#include "conf.h"
#include "pkg_manager.h"
#include "script_handler.h"
#include "fb.h"

int errno;

struct fault_info {
	double timestamp;
	char *filename;
	char *function;
};

/*!
 * \note
 * inst_info describes created instances.
 * every package can holds many instances
 */
struct inst_info {
	enum {
		INST_LOCAL_ONLY,
		INST_REQUEST_TO_CREATE,
		INST_CREATED,
	} state;

	char *filename;
	char *content;
	char *cluster;
	char *category;
	int lb_w;
	int lb_h;
	double priority;
	double timestamp;

	int timeout;
	double period;
	int auto_launch;
	int size_list;

	char *lb_path;
	char *lb_group;

	char *pd_path;
	char *pd_group;

	char *script;

	unsigned int pd_w;
	unsigned int pd_h;
	int pinup;
	int is_pinned_up;
	int text_lb;
	int text_pd;

	struct script_info *lb_script;
	struct script_info *pd_script;

	struct client_node *client;
	struct pkg_info *info;
};

/*!
 * pkg_info describes the loaded package.
 */
struct pkg_info {
	char *pkgname;
	unsigned long fault_count;
	struct fault_info *fault_info;

	/* default values */
	int timeout;
	double period;
	int auto_launch;
	int size_list;
	int secured;

	char *lb_path;
	char *lb_group;

	char *pd_path;
	char *pd_group;

	char *script; /* script type: edje, ... */
	char *abi;

	unsigned int pd_w;
	unsigned int pd_h;
	int pinup;
	int text_lb;
	int text_pd;

	Eina_List *inst_list;
	struct slave_node *slave;
};

static struct info {
	Eina_List *pkg_list;
} s_info = {
	.pkg_list = NULL,
};

static inline struct inst_info *find_instance(struct pkg_info *pkginfo, const char *filename)
{
	Eina_List *l;
	struct inst_info *info;

	EINA_LIST_FOREACH(pkginfo->inst_list, l, info) {
		if (!strcmp(info->filename, filename))
			return info;
	}

	return NULL;
}

static inline struct pkg_info *find_pkginfo(const char *pkgname)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (!strcmp(info->pkgname, pkgname))
			return info;
	}

	return NULL;
}

static inline int delete_instance(struct inst_info *inst)
{
	if (inst->state == INST_CREATED) {
		DbgPrint("Unload instance\n");
		slave_unload_instance(inst->info->slave);
	}

	if (inst->lb_script) {
		script_handler_unload(inst->lb_script, 0);
		script_handler_destroy(inst->lb_script);
	}

	if (inst->pd_script) {
		script_handler_unload(inst->pd_script, 1);
		script_handler_destroy(inst->pd_script);
	}

	(void)util_unlink(inst->filename);

	free(inst->lb_path);
	free(inst->lb_group);

	free(inst->pd_path);
	free(inst->pd_group);

	free(inst->cluster);
	free(inst->category);
	free(inst->filename);
	free(inst->content);

	free(inst->script);
	free(inst);
	return 0;
}

static int slave_del_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = data;
	struct inst_info *inst;
	GVariant *param;

	EINA_LIST_FREE(info->inst_list, inst) {
		param = g_variant_new("(ss)", info->pkgname, inst->filename);
		if (param)
			client_broadcast_command("deleted", param);
		else
			ErrPrint("Failed to create a param\n");

		DbgPrint("delete_instance\n");
		delete_instance(inst);
	}

	return 0;
}

static void renew_ret_cb(const char *funcname, GVariant *result, void *data)
{
	int ret;
	struct inst_info *inst = data;
	GVariant *param;
	struct pkg_info *info = inst->info;

	g_variant_get(result, "(i)", &ret);
	if (ret == 0) {
		inst->state = INST_CREATED;
		slave_load_instance(inst->info->slave);
		return;
	}

	/*!
	 * \note
	 * Failed to re-create an instance.
	 * In this case, delete the instance and send its deleted status to every clients.
	 */
	ErrPrint("Failed to recreate, send delete event to clients (%d)\n", ret);
	param = g_variant_new("(ss)", info->pkgname, inst->filename);
	if (param)
		client_broadcast_command("deleted", param);

	DbgPrint("pkgmgr_delete\n");
	pkgmgr_delete(inst);
}

static int slave_activate_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = data;
	struct inst_info *inst;
	GVariant *param;
	Eina_List *l;

	if (info->fault_info)
		return 0;

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		if (inst->state != INST_LOCAL_ONLY)
			continue;

		param = g_variant_new("(sssiidssiiis)",
				info->pkgname,
				inst->filename,
				inst->content,
				inst->timeout,
				!!inst->lb_path,
				inst->period,
				inst->cluster,
				inst->category,
				inst->is_pinned_up,
				inst->lb_w, inst->lb_h,
				inst->info->abi);
		if (!param) {
			ErrPrint("Failed to create a param\n");
			continue;
		}

		inst->state = INST_REQUEST_TO_CREATE;
		slave_rpc_async_request(slave, info->pkgname, inst->filename, "renew", param, renew_ret_cb, inst);
	}

	return 0;
}

static int slave_deactivate_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = data;

	if (info->fault_info) {
		struct inst_info *inst;
		GVariant *param;

		EINA_LIST_FREE(info->inst_list, inst) {
			param = g_variant_new("(ss)", info->pkgname, inst->filename);
			if (param)
				client_broadcast_command("deleted", param);

			DbgPrint("delete_instance\n");
			delete_instance(inst);
		}

		return 0;
	} else {
		struct inst_info *inst;
		Eina_List *l;

		EINA_LIST_FOREACH(info->inst_list, l, inst) {
			if (inst->state == INST_CREATED) {
				DbgPrint("Unload instance\n");
				slave_unload_instance(inst->info->slave);
			}

			inst->state = INST_LOCAL_ONLY;
		}

		/* Try to re-activate after all deactivate callback is processed */
		return eina_list_count(info->inst_list) ? SLAVE_EVENT_RETURN_REACTIVATE : 0;
	}

	/*!
	 * \note
	 * Couldn't reach to here
	 */
}

static inline int delete_pkginfo(struct pkg_info *info)
{
	struct inst_info *inst;

	slave_event_callback_del(info->slave, SLAVE_EVENT_DELETE, slave_del_cb);
	slave_event_callback_del(info->slave, SLAVE_EVENT_ACTIVATE, slave_activate_cb);
	slave_event_callback_del(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb);

	EINA_LIST_FREE(info->inst_list, inst) {
		DbgPrint("delete_instance\n");
		delete_instance(inst);
	}

	if (info->fault_info) {
		free(info->fault_info->filename);
		free(info->fault_info->function);
		free(info->fault_info);
	}

	free(info->lb_path);
	free(info->lb_group);
	free(info->pd_group);
	free(info->pd_path);
	free(info->pkgname);
	free(info->script);
	slave_unref(info->slave);
	free(info);
	return 0;
}

static struct pkg_info *new_pkginfo(const char *pkgname)
{
	struct pkg_info *info;
	struct item *item;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->pkgname = strdup(pkgname);
	if (!info->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(info);
		return NULL;
	}

	s_info.pkg_list = eina_list_append(s_info.pkg_list, info);

	item = parser_load(info->pkgname);
	if (item) {
		const char *tmp;

		tmp = parser_lb_path(item);
		if (tmp) {
			info->lb_path = strdup(tmp);
			if (!info->lb_path) {
				ErrPrint("Error: %s\n", strerror(errno));
			} else {
				tmp = parser_lb_group(item);
				if (tmp) {
					info->lb_group = strdup(tmp);
					if (!info->lb_group)
						ErrPrint("Error: %s\n", strerror(errno));
				}
			}
		} else {
			info->lb_path = NULL; /* Livebox has not to include the EDJE file */
		}

		tmp = parser_pd_path(item);
		if (tmp) {
			info->pd_path = strdup(tmp);
			if (!info->pd_path) {
				ErrPrint("Error: %s\n", strerror(errno));
			} else {
				tmp = parser_pd_group(item);
				if (tmp)
					info->pd_group = strdup(tmp);
				else
					info->pd_group = strdup(DEFAULT_GROUP);

				if (!info->pd_group)
					ErrPrint("Error: %s\n", strerror(errno));
			}
		} else {
			info->pd_path = NULL;
		}

		tmp = parser_script(item);
		info->script = tmp ? strdup(tmp) : strdup(DEFAULT_SCRIPT);
		if (!info->script)
			ErrPrint("Heap: %s\n", strerror(errno));

		tmp = parser_abi(item);
		info->abi = tmp ? strdup(tmp) : strdup(DEFAULT_ABI);
		if (!info->abi)
			ErrPrint("Error: %s\n", strerror(errno));

		info->timeout = parser_timeout(item);

		info->period = parser_period(item);
		if (info->period < 0.0f)
			info->period = 0.0f;
		else if (info->period > 0.0f && info->period < MINIMUM_PERIOD)
			info->period = MINIMUM_PERIOD;

		info->size_list = parser_size(item);
		info->auto_launch = parser_auto_launch(item);
		info->secured = parser_secured(item);
		info->text_pd = parser_text_pd(item);
		info->text_lb = parser_text_lb(item);
		parser_get_pdsize(item, &info->pd_w, &info->pd_h);
		info->pinup = parser_pinup(item);

		parser_unload(item);
	} else {
		info->size_list = 0x01; /* Default */

		info->script = strdup(DEFAULT_SCRIPT);
		if (!info->script)
			ErrPrint("Error: %s\n", strerror(errno));

		info->abi = strdup(DEFAULT_ABI);
		if (!info->script)
			ErrPrint("Error: %s\n", strerror(errno));

		info->pd_w = g_conf.width;
		info->pd_h = g_conf.height / 4;
		info->pinup = 1;
	}

	if (!info->secured) {
		DbgPrint("Non-secured livebox is created for %s\n", info->pkgname);
		info->slave = slave_find_available();
	}

	if (!info->slave) {
		char slavename[BUFSIZ];
		snprintf(slavename, sizeof(slavename), "%lf", util_get_timestamp());

		DbgPrint("Create a new slave %s for %s, (secured: %d\n", slavename, info->pkgname, info->secured);
		info->slave = slave_create(slavename, info->secured);
	}

	if (!info->slave) {
		ErrPrint("Failed to create a new slave\n");
		delete_pkginfo(info);
		return NULL;
	}

	slave_ref(info->slave);
	slave_load_package(info->slave);

	slave_event_callback_add(info->slave, SLAVE_EVENT_DELETE, slave_del_cb, info);
	slave_event_callback_add(info->slave, SLAVE_EVENT_ACTIVATE, slave_activate_cb, info);
	slave_event_callback_add(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb, info);
	return info;
}

static inline void prepare_fb(struct inst_info *inst)
{
	if (inst->lb_path) {
		inst->lb_script = script_handler_create(inst, inst->lb_path, inst->lb_group, inst->lb_w, inst->lb_h);
		if (script_handler_load(inst->lb_script, 0) < 0) {
			script_handler_destroy(inst->lb_script);
			inst->lb_script = NULL;
		}
	}

	if (inst->pd_path) {
		inst->pd_script = script_handler_create(inst, inst->pd_path, inst->pd_group, inst->pd_w, inst->pd_h);
		if (!inst->pd_script && inst->lb_script) {
			ErrPrint("Failed to load PD script\n");
			script_handler_unload(inst->lb_script, 0);
			script_handler_destroy(inst->lb_script);
			inst->lb_script = NULL;
		}
	}
}

void pkgmgr_pd_updated_by_inst(struct inst_info *inst, const char *descfile)
{
	GVariant *param;

	/*!
	 * \note
	 * the description file can be NULL
	 * when a pd is written as EDJE
	 */
	if (!descfile)
		descfile = inst->filename;

	param = g_variant_new("(sssii)",
			inst->info->pkgname, inst->filename, descfile, inst->pd_w, inst->pd_h);
	if (param)
		client_broadcast_command("pd_updated", param);
	else
		ErrPrint("Failed to create param (%s - %s)\n", inst->info->pkgname, inst->filename);
}

void pkgmgr_lb_updated_by_inst(struct inst_info *inst)
{
	GVariant *param;

	param = g_variant_new("(ssiid)",
				inst->info->pkgname, inst->filename,
				inst->lb_w, inst->lb_h, inst->priority);
	if (param)
		client_broadcast_command("lb_updated", param);
	else
		ErrPrint("Failed to create param (%s - %s)\n", inst->info->pkgname, inst->filename);

}

int pkgmgr_set_fault(const char *pkgname, const char *filename, const char *funcname)
{
	struct pkg_info *info;
	struct fault_info *fault;

	info = find_pkginfo(pkgname);
	if (!info) {
		ErrPrint("Package %s is not found\n", pkgname);
		return -ENOENT;
	}

	info->fault_count++;

	fault = malloc(sizeof(*fault));
	if (!fault) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (filename) {
		fault->filename = strdup(filename);
		if (!fault->filename) {
			ErrPrint("Error: %s\n", strerror(errno));
			free(fault);
			return -ENOMEM;
		}
	} else {
		fault->filename = NULL;
	}

	if (funcname) {
		fault->function = strdup(funcname);
		if (!fault->function) {
			ErrPrint("Error: %s\n", strerror(errno));
			free(fault->filename);
			free(fault);
			return -ENOMEM;
		}
	} else {
		fault->function = NULL;
	}

	fault->timestamp = util_get_timestamp();

	if (info->fault_info) {
		/* Just for debugging and exceptional case,
		 * This is not possible to occur */
		DbgPrint("Previous fault info will be overrided with new info\n");
		DbgPrint("Filename: %s\n", info->fault_info->filename);
		DbgPrint("Function: %s\n", info->fault_info->function);
		free(info->fault_info->filename);
		free(info->fault_info->function);
		free(info->fault_info);
	}

	info->fault_info = fault;
	return 0;
}

int pkgmgr_clear_fault(const char *pkgname)
{
	struct pkg_info *info;

	info = find_pkginfo(pkgname);
	if (!info)
		return -ENOENT;

	if (!info->fault_info)
		return -EINVAL;

	free(info->fault_info->filename);
	free(info->fault_info->function);
	free(info->fault_info);
	info->fault_info = NULL;
	return 0;
}

int pkgmgr_get_fault(const char *pkgname, double *timestamp, const char **filename, const char **funcname)
{
	struct pkg_info *info;

	info = find_pkginfo(pkgname);
	if (!info)
		return -EINVAL;

	if (!info->fault_info)
		return -ENOENT;

	*timestamp = info->fault_info->timestamp;
	*filename = info->fault_info->filename;
	*funcname = info->fault_info->function;
	return 0;
}

int pkgmgr_is_fault(const char *pkgname)
{
	struct pkg_info *info;

	info = find_pkginfo(pkgname);
	if (!info)
		return 0;

	return !!info->fault_info;
}

struct slave_node *pkgmgr_slave(const char *pkgname)
{
	struct pkg_info *info;

	info = find_pkginfo(pkgname);
	if (!info)
		return NULL;

	return info->slave;
}

const char *pkgmgr_find_by_secure_slave(struct slave_node *slave)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave && slave_is_secured(info->slave))
			return info->pkgname;
	}

	return NULL;
}

struct inst_info *pkgmgr_find(const char *pkgname, const char *filename)
{
	struct pkg_info *info;
	struct inst_info *inst;

	if (!pkgname || !filename)
		return NULL;

	info = find_pkginfo(pkgname);
	if (!info)
		return NULL;

	inst = find_instance(info, filename);

	return inst;
}

static inline int send_created_to_client(struct inst_info *inst)
{
	GVariant *param;

	prepare_fb(inst);

	param = g_variant_new("(dsssiiiissssidiiiiid)", 
			inst->timestamp,
			inst->info->pkgname, inst->filename, inst->content,
			inst->lb_w, inst->lb_h, inst->pd_w, inst->pd_h,
			inst->cluster, inst->category,
			fb_filename(script_handler_fb(inst->lb_script)),
			fb_filename(script_handler_fb(inst->pd_script)),
			inst->auto_launch,
			inst->priority,
			inst->size_list,
			!!inst->client,
			inst->pinup,
			inst->text_lb,
			inst->text_pd,
			inst->period);

	if (param)
		client_broadcast_command("created", param);

	return 0;
}

static void new_ret_cb(const char *funcname, GVariant *result, void *data)
{
	struct inst_info *inst = data;
	struct inst_info *new_inst;
	double timestamp;
	char *filename;
	double priority;
	int w;
	int h;
	int ret;

	g_variant_get(result, "(iiid)", &ret, &w, &h, &priority);
	g_variant_unref(result);

	switch (ret) {
	case 1: /* need to create */
		/*!
		 * \note
		 * Send create livebox again
		 * re-use "inst" from here
		 */
		timestamp = util_get_timestamp();
		filename = util_new_filename(timestamp);
		new_inst = pkgmgr_new(inst->client, timestamp,
					inst->info->pkgname, filename,
					inst->content, inst->cluster, inst->category, inst->period);
		free(filename);
	case 0: /* no need to create */
		inst->lb_w = w;
		inst->lb_h = h;
		inst->priority = priority;

		send_created_to_client(inst);
		inst->state = INST_CREATED;
		slave_load_instance(inst->info->slave);
		break;
	default: /* error */
		/*\note
		 * If the current instance is created by the client,
		 * send the deleted event or just delete an instance in the master
		 * It will be cared by the "create_ret_cb"
		 */
		if (inst->client) {
			GVariant *param;
			/* Okay, the client wants to know about this */
			param = g_variant_new("(ss)", inst->info->pkgname, inst->filename);
			if (param)
				client_push_command(inst->client, "deleted", param);
		}

		DbgPrint("pkgmgr_delete\n");
		pkgmgr_delete(inst);
		break;
	}
}

static inline void send_to_slave(struct inst_info *inst)
{
	GVariant *param;

	param = g_variant_new("(sssiidssiis)",
			inst->info->pkgname,
			inst->filename,
			inst->content,
			inst->timeout,
			!!inst->lb_path,
			inst->period,
			inst->cluster,
			inst->category,
			inst->is_pinned_up,
			!!inst->client,
			inst->info->abi);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return;
	}

	/*!
	 * \note
	 * Try to activate a slave if it is not activated
	 */
	slave_activate(inst->info->slave);

	(void)slave_rpc_async_request(inst->info->slave, inst->info->pkgname, inst->filename, "new", param, new_ret_cb, inst);
}

struct inst_info *pkgmgr_new(struct client_node *client, double timestamp, const char *pkgname, const char *filename, const char *content, const char *cluster, const char *category, double period)
{
	struct pkg_info *info;
	struct inst_info *inst;

	info = find_pkginfo(pkgname);
	if (!info) {
		info = new_pkginfo(pkgname);
		if (!info) {
			ErrPrint("Failed to create new pkginfo\n");
			return NULL;
		}
	} else {
		if (info->fault_info) {
			ErrPrint("Access denied to fault package\n");
			return NULL;
		}
	}

	inst = find_instance(info, filename);
	if (inst)
		return inst;

	inst = calloc(1, sizeof(*inst));
	if (!inst) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	inst->filename = strdup(filename);
	if (!inst->filename) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst);
		return NULL;
	}

	if (content) {
		inst->content = strdup(content);
		if (!inst->content) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(inst->filename);
			free(inst);
			return NULL;
		}
	} else {
		inst->content = NULL;
	}

	inst->timestamp = timestamp;

	inst->cluster = strdup(cluster);
	if (!inst->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst->content);
		free(inst->filename);
		free(inst);
		return NULL;
	}

	inst->category = strdup(category);
	if (!inst->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(inst->cluster);
		free(inst->content);
		free(inst->filename);
		free(inst);
		return NULL;
	}

	inst->info = info;

	inst->timeout = info->timeout;
	if (period < 0.0f) {
		inst->period = info->period;
	} else {
		inst->period = period;
	}
	inst->size_list = info->size_list;
	inst->pinup = info->pinup;
	inst->is_pinned_up = 0;
	inst->text_lb = info->text_lb;
	inst->text_pd = info->text_pd;
	inst->auto_launch = info->auto_launch;

	if (info->lb_path) {
		inst->lb_path = strdup(info->lb_path);
		if (!inst->lb_path) {
			ErrPrint("Heap: %s\n", strerror(errno));
		} else {
			inst->lb_group = strdup(info->lb_group);
			if (!inst->lb_group) {
				ErrPrint("Heap: %s\n", strerror(errno));
				free(inst->lb_path);
				inst->lb_path = NULL;
			}
		}
	}

	if (info->pd_path) {
		inst->pd_path = strdup(info->pd_path);
		if (!inst->pd_path) {
			ErrPrint("Heap: %s\n", strerror(errno));
		} else if (info->pd_group) {
			inst->pd_group = strdup(info->pd_group);
			if (!inst->pd_group) {
				ErrPrint("Heap: %s\n", strerror(errno));
				free(inst->pd_path);
				inst->pd_path = NULL;
			}	
		}
	}

	inst->script = strdup(info->script);
	if (!inst->script)
		ErrPrint("Heap: %s\n", strerror(errno));

	inst->pd_w = info->pd_w;
	inst->pd_h = info->pd_h;
	inst->client = client;
	inst->state = INST_REQUEST_TO_CREATE; /* send_to_slave */
	info->inst_list = eina_list_append(info->inst_list, inst);

	send_to_slave(inst);
	return inst;
}

struct client_node *pkgmgr_client(struct inst_info *inst)
{
	return inst->client;
}

int pkgmgr_delete(struct inst_info *inst)
{
	struct pkg_info *info;

	info = inst->info;

	info->inst_list = eina_list_remove(info->inst_list, inst);
	DbgPrint("delete_instance\n");
	delete_instance(inst);

	/*! \note
	 * Do not delete package info event if it has instances
	 * if (!info->inst_list)
	 */
	return 0;
}

const char *pkgmgr_lb_path(struct inst_info *inst)
{
	return inst->lb_path;
}

const char *pkgmgr_lb_group(struct inst_info *inst)
{
	return inst->lb_group;
}

const char *pkgmgr_pd_path(struct inst_info *inst)
{
	return inst->pd_path;
}

const char *pkgmgr_pd_group(struct inst_info *inst)
{
	return inst->pd_group;
}

const char *pkgmgr_filename(struct inst_info *inst)
{
	return inst->filename;
}

const char *pkgmgr_cluster(struct inst_info *inst)
{
	return inst->cluster;
}

const char *pkgmgr_category(struct inst_info *inst)
{
	return inst->category;
}

int pkgmgr_timeout(struct inst_info *inst)
{
	return inst->timeout;
}

double pkgmgr_period(struct inst_info *inst)
{
	return inst->period;
}

void pkgmgr_set_period(struct inst_info *inst, double period)
{
	inst->period = period;
}

const char *pkgmgr_script(struct inst_info *inst)
{
	return inst->script;
}

int pkgmgr_inform_pkglist(struct client_node *client)
{
	struct pkg_info *info;
	Eina_List *l;

	struct inst_info *inst;
	Eina_List *i_l;

	GVariant *param;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->fault_info) {
			const char *pkgname;
			const char *filename;
			const char *func;

			pkgname = info->pkgname;
			filename = info->fault_info->filename;
			if (!filename)
				filename = "";

			func = info->fault_info->function;
			if (!func)
				func = "";

			/* Send all fault package list to the new client */
			param = g_variant_new("(sss)", pkgname, filename, func);
			if (param)
				client_push_command(client, "fault_package", param);

			continue;
		}

		EINA_LIST_FOREACH(info->inst_list, i_l, inst) {
			/* Send all instance list to the new client */
			param = g_variant_new("(dsssiiiissssidiiiiid)", 
					inst->timestamp,
					info->pkgname, inst->filename, inst->content,
					inst->lb_w, inst->lb_h, inst->pd_w, inst->pd_h,
					inst->cluster, inst->category,
					fb_filename(script_handler_fb(inst->lb_script)),
					fb_filename(script_handler_fb(inst->pd_script)),
					inst->auto_launch,
					inst->priority,
					inst->size_list,
					!!inst->client,
					inst->pinup,
					inst->text_lb,
					inst->text_pd,
					inst->period);
			if (param)
				client_push_command(client, "created", param);
		}
	}

	return 0;
}

int pkgmgr_deleted(const char *pkgname, const char *filename)
{
	struct pkg_info *info;
	struct inst_info *inst;
	GVariant *param;

	info = find_pkginfo(pkgname);
	if (!info)
		return -ENOENT;

	inst = find_instance(info, filename);
	if (!inst)
		return -ENOENT;

	param = g_variant_new("(ss)", pkgname, filename);
	if (param)
		client_broadcast_command("deleted", param);

	DbgPrint("pkgmgr_delete\n");
	pkgmgr_delete(inst);
	return 0;
}

int pkgmgr_lb_updated(const char *pkgname, const char *filename, int w, int h, double priority)
{
	struct pkg_info *info;
	struct inst_info *inst;

	info = find_pkginfo(pkgname);
	if (!info)
		return -ENOENT;

	inst = find_instance(info, filename);
	if (!inst)
		return -ENOENT;

	inst->lb_w = w;
	inst->lb_h = h;
	inst->priority = priority;

	pkgmgr_lb_updated_by_inst(inst);

	return 0;
}

int pkgmgr_pd_updated(const char *pkgname, const char *filename, const char *descfile, int w, int h)
{
	struct pkg_info *info;
	struct inst_info *inst;

	info = find_pkginfo(pkgname);
	if (!info)
		return -ENOENT;

	inst = find_instance(info, filename);
	if (!inst)
		return -ENOENT;

	inst->pd_w = w;
	inst->pd_h = h;

	pkgmgr_pd_updated_by_inst(inst, descfile);

	return 0;
}

int pkgmgr_init(void)
{
	return 0;
}

int pkgmgr_fini(void)
{
	Eina_List *l;
	Eina_List *t;
	struct pkg_info *info;

	EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, t, info) {
		s_info.pkg_list = eina_list_remove_list(s_info.pkg_list, l);
		delete_pkginfo(info);
	}

	return 0;
}

const char *pkgmgr_name(struct inst_info *inst)
{
	return inst->info->pkgname;
}

const char *pkgmgr_content(struct inst_info *inst)
{
	return inst->content;
}

const char *pkgmgr_name_by_info(struct pkg_info *info)
{
	return info->pkgname;
}

void *pkgmgr_lb_script(struct inst_info *inst)
{
	return inst->lb_script;
}

void *pkgmgr_pd_script(struct inst_info *inst)
{
	return inst->pd_script;
}

int pkgmgr_set_pinup(struct inst_info *inst, int flag)
{
	GVariant *param;
	int ret;

	if (flag && !inst->pinup)
		return -EINVAL;

	if (inst->is_pinned_up == flag)
		return 0;

	inst->is_pinned_up = flag;

	param = g_variant_new("(ssi)", inst->info->pkgname, inst->filename, flag);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	ret = slave_rpc_async_request(inst->info->slave, inst->info->pkgname, inst->filename, "pinup", param, NULL, NULL);
	return ret;
}

int pkgmgr_pinup(struct inst_info *inst)
{
	return inst->is_pinned_up;
}

int pkgmgr_load_pd(struct inst_info *inst)
{
	int ret;

	if (!inst->pd_script)
		return -EINVAL;

	ret = script_handler_load(inst->pd_script, 1);
	return ret;
}

int pkgmgr_unload_pd(struct inst_info *inst)
{
	int ret;

	if (!inst->pd_script)
		return -EINVAL;

	ret = script_handler_unload(inst->pd_script, 1);
	return ret;
}

int pkgmgr_is_secured(const char *pkgname)
{
	struct pkg_info *info;

	info = find_pkginfo(pkgname);
	if (!info)
		return 0;

	return info->secured;
}

int pkgmgr_text_pd(struct inst_info *inst)
{
	return inst->text_pd;
}

int pkgmgr_text_lb(struct inst_info *inst)
{
	return inst->text_lb;
}

int pkgmgr_update_size(struct inst_info *inst, int w, int h, int is_pd)
{
	if (is_pd) {
		inst->pd_w = w;
		inst->pd_h = h;
	} else {
		inst->lb_w = w;
		inst->lb_h = h;
	}

	return 0;
}

int pkgmgr_get_size(struct inst_info *inst, int *w, int *h, int is_pd)
{
	if (is_pd) {
		*w = inst->pd_w;
		*h = inst->pd_h;
	} else {
		*w = inst->lb_w;
		*h = inst->lb_h;
	}

	return 0;
}

const char *pkgmgr_abi(struct inst_info *inst)
{
	return inst->info->abi;
}

void pkgmgr_clear_slave_info(struct slave_node *slave)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave)
			info->slave = NULL;
	}
}

/* End of a file */
