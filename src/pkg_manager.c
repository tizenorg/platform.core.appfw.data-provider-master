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
#include "slave_manager.h"
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
	int pinup_supported;
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
	int pinup_supported;
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
	struct pkg_info *info;

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

	info = inst->info;

	free(inst);

	if (!info || !info->slave)
		return 0;

	if (slave_unref(info->slave) <= 0)
		slave_destroy(info->slave);

	return 0;
}

static inline struct pkg_info *new_pkginfo(const char *pkgname)
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
		info->pinup_supported = parser_pinup(item);

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
		info->pinup_supported = 1;
	}

	return info;
}

static inline int delete_pkginfo(struct pkg_info *info)
{
	struct inst_info *inst;

	EINA_LIST_FREE(info->inst_list, inst) {
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
	free(info);
	return 0;
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

int pkgmgr_set_slave(const char *pkgname, struct slave_node *slave)
{
	struct pkg_info *info;

	info = find_pkginfo(pkgname);
	if (!info) {
		ErrPrint("Failed to find correct pkginfo : %s\n", pkgname);
		return -ENOENT;
	}

	if (info->slave) {
		ErrPrint("Already specified [%s:%d]\n", slave_name(info->slave), slave_pid(info->slave));
		return -EBUSY;
	}

	info->slave = slave;
	return 0;
}

int pkgmgr_renew_by_slave(struct slave_node *slave, int (*cb)(struct slave_node *, struct inst_info *, void *), void *data)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave) {
			Eina_List *il;
			Eina_List *in;
			struct inst_info *inst;
			int ret;

			EINA_LIST_FOREACH_SAFE(info->inst_list, il, in, inst) {
				ret = cb(slave, inst, data);
				if (ret != EXIT_SUCCESS)
					return -EFAULT;
			}
		}
	}

	return 0;
}

int pkgmgr_renew_by_pkg(const char *pkgname, int (*cb)(struct slave_node *, struct inst_info *, void *), void *data)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (!strcmp(info->pkgname, pkgname)) {
			Eina_List *il;
			Eina_List *in;
			struct inst_info *inst;
			int ret;

			EINA_LIST_FOREACH_SAFE(info->inst_list, il, in, inst) {
				ret = cb(info->slave, inst, data);
				if (ret != EXIT_SUCCESS)
					return -EFAULT;
			}
		}
	}

	return 0;
}

const char *pkgmgr_find_by_slave(struct slave_node *slave)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave)
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

struct inst_info *pkgmgr_new(double timestamp, const char *pkgname, const char *filename, const char *content, const char *cluster, const char *category)
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
	}

	if (pkgmgr_is_fault(pkgname)) {
		ErrPrint("Fault package is not able to be created\n");
		return NULL;
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
	inst->period = info->period;
	inst->size_list = info->size_list;
	inst->pinup_supported = info->pinup_supported;
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

	info->inst_list = eina_list_append(info->inst_list, inst);

	if (info->slave) {
		slave_ref(info->slave);
		return inst;
	}

	if (!info->secured)
		info->slave = slave_find_usable();

	if (!info->slave) {
		char slavename[BUFSIZ];
		snprintf(slavename, sizeof(slavename), "%lf", util_get_timestamp());
		info->slave = slave_create(slavename, info->secured);
	}

	if (!info->slave) {
		ErrPrint("Failed to get proper slave object\n");
		pkgmgr_delete(inst);
		return NULL;
	}

	slave_ref(info->slave);
	return inst;
}

int pkgmgr_delete_by_slave(struct slave_node *slave)
{
	Eina_List *p_l;
	Eina_List *p_n;
	struct pkg_info *info;

	struct inst_info *inst;

	GVariant *param;

	EINA_LIST_FOREACH_SAFE(s_info.pkg_list, p_l, p_n, info) {
		if (info->slave != slave)
			continue;

		EINA_LIST_FREE(info->inst_list, inst) {
			param = g_variant_new("(ss)", info->pkgname, inst->filename);
			if (param)
				client_broadcast_command("deleted", param);
			else
				ErrPrint("Failed to create a param\n");

			delete_instance(inst);
		}

		/* \NOTE
		 * Do not delete a package info even if it has no instances
		 * if (!info->inst_list)
		 */
	}

	return 0;
}

int pkgmgr_delete_by_client(struct client_node *client)
{
	Eina_List *p_l;
	Eina_List *p_n;
	struct pkg_info *info;

	Eina_List *i_l;
	Eina_List *i_n;
	struct inst_info *inst;

	GVariant *param;

	EINA_LIST_FOREACH_SAFE(s_info.pkg_list, p_l, p_n, info) {
		if (!info->inst_list)
			continue;

		EINA_LIST_FOREACH_SAFE(info->inst_list, i_l, i_n, inst) {
			if (inst->client != client)
				continue;

			info->inst_list = eina_list_remove_list(info->inst_list, i_l);
			if (!info->slave) {
				delete_instance(inst);
				continue;
			}

			param = g_variant_new("(ss)", info->pkgname, inst->filename);
			if (param) {
				/* Send a command to the proper slave, to delete an instance */
				(void)slave_push_command(info->slave,
						info->pkgname, inst->filename, "delete",
						g_variant_ref(param), NULL, NULL);
				/* Broadcast deleted livebox to every client to handling it correctly */
				client_broadcast_command("deleted", param);
			} else {
				ErrPrint("Failed to create param\n");
			}

			delete_instance(inst);
		}

		/* \NOTE
		 * Do not delete a package info even if it has no instances
		 * if (!info->inst_list)
		 */
	}

	return 0;
}

void pkgmgr_set_info(struct inst_info *inst, int w, int h, double priority)
{
	inst->lb_w = w;
	inst->lb_h = h;
	inst->priority = priority;
}

int pkgmgr_set_client(struct inst_info *inst, struct client_node *client)
{
	if (inst->client)
		return -EBUSY;

	inst->client = client;
	return 0;
}

struct client_node *pkgmgr_client(struct inst_info *inst)
{
	return inst->client;
}

double pkgmgr_timestamp(struct inst_info *inst)
{
	return inst->timestamp;
}

int pkgmgr_delete(struct inst_info *inst)
{
	struct pkg_info *info;

	info = inst->info;

	info->inst_list = eina_list_remove(info->inst_list, inst);
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
					pkgmgr_timestamp(inst),
					info->pkgname, inst->filename, inst->content,
					inst->lb_w, inst->lb_h, inst->pd_w, inst->pd_h,
					inst->cluster, inst->category,
					fb_filename(script_handler_fb(inst->lb_script)),
					fb_filename(script_handler_fb(inst->pd_script)),
					inst->auto_launch,
					inst->priority,
					inst->size_list,
					!!inst->client,
					inst->pinup_supported,
					inst->text_lb,
					inst->text_pd,
					inst->period);
			if (param)
				client_push_command(client, "created", param);
		}
	}

	return 0;
}

int pkgmgr_created(const char *pkgname, const char *filename)
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

	prepare_fb(inst);

	param = g_variant_new("(dsssiiiissssidiiiiid)", 
			pkgmgr_timestamp(inst),
			pkgname, filename, inst->content,
			inst->lb_w, inst->lb_h, inst->pd_w, inst->pd_h,
			inst->cluster, inst->category,
			fb_filename(script_handler_fb(inst->lb_script)),
			fb_filename(script_handler_fb(inst->pd_script)),
			inst->auto_launch,
			inst->priority,
			inst->size_list,
			!!inst->client,
			inst->pinup_supported,
			inst->text_lb,
			inst->text_pd,
			inst->period);

	if (param)
		client_broadcast_command("created", param);

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

	if (flag && !inst->pinup_supported)
		return -EINVAL;

	if (inst->is_pinned_up == flag)
		return 0;

	inst->is_pinned_up = flag;

	param = g_variant_new("(ssi)", inst->info->pkgname, inst->filename, flag);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	ret = slave_push_command(inst->info->slave, inst->info->pkgname, inst->filename, "pinup", param, NULL, NULL);
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
