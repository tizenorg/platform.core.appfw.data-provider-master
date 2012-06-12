#include <stdio.h>
#include <errno.h>
#include <string.h> /* strcmp */
#include <stdlib.h> /* free */
#include <libgen.h> /* basename */

#include <dlog.h>
#include <Eina.h>
#include <Ecore_Evas.h>

#include "debug.h"
#include "util.h"
#include "parser.h"
#include "conf.h"
#include "slave_life.h"
#include "client_life.h"
#include "fault_manager.h"
#include "package.h"
#include "instance.h"
#include "fb.h"
#include "script_handler.h"
#include "group.h"

int errno;

struct fault_info {
	double timestamp;
	char *filename;
	char *function;
};

/*!
 * pkg_info describes the loaded package.
 */
struct pkg_info {
	char *pkgname;

	struct {
		enum lb_type type;

		union {
			struct {
				char *path;
				char *group;
			} script;

			struct {
				/*!< Reserved for future use */
			} file;

			struct {
				/*!< Reserved for future use */
			} text;
		} info;

		unsigned int size_list;
		int auto_launch;
		int pinup;
	} lb;

	struct {
		enum pd_type type;

		union {
			struct {
				char *path;
				char *group;
			} script;

			struct {
				/*!< Reserved for future use */
			} text;
		} info;

		unsigned int width;
		unsigned int height;
	} pd;

	int timeout;
	double period;
	int secured;
	char *script; /* script type: edje, ... */
	char *abi;

	int fault_count;
	struct fault_info *fault_info;

	struct slave_node *slave;
	int refcnt;

	Eina_List *inst_list;
};

static struct {
	Eina_List *pkg_list;
} s_info = {
	.pkg_list = NULL,
};

static int slave_activated_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = data;
	struct inst_info *inst;
	Eina_List *l;
	Eina_List *n;
	int cnt;

	if (!slave_is_faulted(slave)) {
		DbgPrint("No need to recover state of instances of %s\n", package_name(info));
		return 0;
	}

	cnt = 0;
	EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
		instance_recover_state(inst);
		cnt++;
	}

	DbgPrint("Recover state for %d instances of %s\n", cnt, package_name(info));
	return 0;
}

static int slave_deactivated_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = data;
	struct inst_info *inst;
	Eina_List *l;
	Eina_List *n;
	int cnt = 0;

	if (info->fault_info) {
		EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
			instance_faulted(inst);
		}
	} else {
		EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
			/*!
			 * instance_deactivated will call the slave_unload_instance.
			 * if the loaded instance counter meets 0,
			 * the slave will be deactivated.
			 * so we should not call the instance activate function
			 * from here.
			 *
			 * activate slave when the slave is reactivated
			 */
			cnt++;
		}
	}

	return cnt ? SLAVE_NEED_TO_REACTIVATE : 0;
}

static inline void destroy_package(struct pkg_info *info)
{
	s_info.pkg_list = eina_list_remove(s_info.pkg_list, info);

	group_del_livebox(info->pkgname);

	if (info->lb.type == LB_TYPE_SCRIPT) {
		free(info->lb.info.script.path);
		free(info->lb.info.script.group);
	}

	if (info->pd.type == PD_TYPE_SCRIPT) {
		free(info->pd.info.script.path);
		free(info->pd.info.script.group);
	}

	free(info->script);
	free(info->abi);
	free(info->pkgname);

	if (info->slave) {
		slave_unload_package(info->slave);
		slave_event_callback_del(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
		slave_event_callback_del(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);
	}

	free(info);
}

static inline int load_conf(struct pkg_info *info, const char *pkgname)
{
	struct parser *parser;
	const char *str;
	const char *group;

	parser = parser_load(info->pkgname);
	if (!parser) {
		info->lb.size_list = 0x01; /* Default */

		info->script = strdup(DEFAULT_SCRIPT);
		if (!info->script) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return -ENOMEM;
		}

		info->abi = strdup(DEFAULT_ABI);
		if (!info->abi) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(info->script);
			info->script = NULL;
			return -ENOMEM;
		}

		info->pd.width = g_conf.width;
		info->pd.height = g_conf.height >> 2;
		info->lb.pinup = 1;
		return 0;
	}

	info->lb.type = LB_TYPE_FILE;
	if (parser_text_lb(parser)) {
		info->lb.type = LB_TYPE_TEXT;
	} else {
		str = parser_lb_path(parser);
		if (str) {
			info->lb.type = LB_TYPE_SCRIPT;

			info->lb.info.script.path = strdup(str);
			if (!info->lb.info.script.path) {
				ErrPrint("Heap: %s\n", strerror(errno));
				parser_unload(parser);
				return -ENOMEM;
			}

			str = parser_lb_group(parser);
			if (str) {
				info->lb.info.script.group = strdup(str);
				if (!info->lb.info.script.group) {
					ErrPrint("Heap: %s\n", strerror(errno));
					free(info->lb.info.script.path);
					parser_unload(parser);
					return -ENOMEM;
				}
			}
		}
	}

	if (parser_text_pd(parser)) {
		info->pd.type = PD_TYPE_TEXT;
	} else {
		str = parser_pd_path(parser);
		if (str) {
			info->pd.type = PD_TYPE_SCRIPT;
			info->pd.info.script.path = strdup(str);
			if (!info->pd.info.script.path) {
				ErrPrint("Heap: %s\n", strerror(errno));
				if (info->lb.type == LB_TYPE_SCRIPT) {
					free(info->lb.info.script.path);
					free(info->lb.info.script.group);
				}
				parser_unload(parser);
				return -ENOMEM;
			}

			str = parser_pd_group(parser);
			if (str) {
				info->pd.info.script.group = strdup(str);
				if (!info->pd.info.script.group) {
					ErrPrint("Heap: %s\n", strerror(errno));
					free(info->pd.info.script.path);
					if (info->lb.type == LB_TYPE_SCRIPT) {
						free(info->lb.info.script.path);
						free(info->lb.info.script.group);
					}
					parser_unload(parser);
					return -ENOMEM;
				}
			}
		}
	}

	str = parser_script(parser);
	str = str ? str : DEFAULT_SCRIPT;
	info->script = strdup(str);
	if (!info->script) {
		ErrPrint("Heap: %s\n", strerror(errno));
		if (info->pd.type == PD_TYPE_SCRIPT) {
			free(info->pd.info.script.path);
			free(info->pd.info.script.group);
		}

		if (info->lb.type == LB_TYPE_SCRIPT) {
			free(info->lb.info.script.path);
			free(info->lb.info.script.group);
		}

		parser_unload(parser);
		return -ENOMEM;
	}

	str = parser_abi(parser);
	str = str ? str : DEFAULT_ABI;
	info->abi = strdup(str);
	if (!info->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(info->script);
		if (info->pd.type == PD_TYPE_SCRIPT) {
			free(info->pd.info.script.path);
			free(info->pd.info.script.group);
		}

		if (info->lb.type == LB_TYPE_SCRIPT) {
			free(info->lb.info.script.path);
			free(info->lb.info.script.group);
		}
		parser_unload(parser);
		return -ENOMEM;
	}

	info->timeout = parser_timeout(parser);

	info->period = parser_period(parser);
	if (info->period < 0.0f)
		info->period = 0.0f;
	else if (info->period > 0.0f && info->period < MINIMUM_PERIOD)
		info->period = MINIMUM_PERIOD;

	info->lb.size_list = parser_size(parser);
	info->lb.auto_launch = parser_auto_launch(parser);
	info->secured = parser_secured(parser);
	info->lb.pinup = parser_pinup(parser);

	parser_get_pdsize(parser, &info->pd.width, &info->pd.height);

	group = parser_group_str(parser);
	if (group && group_add_livebox(group, pkgname) < 0)
		ErrPrint("Failed to build cluster tree for %s{%s}\n", pkgname, group);

	parser_unload(parser);
	return 0;
}

struct pkg_info *package_create(const char *pkgname)
{
	struct pkg_info *info;

	if (util_validate_livebox_package(pkgname) < 0)
		return NULL;

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

	if (load_conf(info, pkgname) < 0) {
		free(info->pkgname);
		free(info);
		return NULL;
	}

	if (!info->secured)
		info->slave = slave_find_available();

	package_ref(info);

	if (!info->slave) {
		char *slavename;
		slavename = util_slavename();
		if (!slavename) {
			package_destroy(info);
			return NULL;
		}

		DbgPrint("New slave name is %s assigned for %s\n", slavename, pkgname);
		info->slave = slave_create(slavename, info->secured);
		free(slavename);
		/*!
		 * \note
		 * Slave is not activated yet.
		 */
	} else {
		DbgPrint("Slave %s is assigned for %s\n", slave_name(info->slave), pkgname);
	}

	if (!info->slave) {
		/*!
		 * \note
		 * package_destroy will try to remove "info" from the pkg_list.
		 * but we didn't add this to it yet.
		 * If the list method couldn't find an "info" from the list,
		 * it just do nothing so I'll leave this.
		 */
		package_destroy(info);
		return NULL;
	}

	slave_load_package(info->slave);
	slave_event_callback_add(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
	slave_event_callback_add(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);

	s_info.pkg_list = eina_list_append(s_info.pkg_list, info);
	return info;
}

int package_destroy(struct pkg_info *info)
{
	package_unref(info);
	return 0;
}

struct pkg_info *package_find(const char *pkgname)
{
	Eina_List *l;
	struct pkg_info *info;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (!strcmp(info->pkgname, pkgname))
			return info;
	}

	return NULL;
}

struct inst_info *package_find_instance_by_id(const char *pkgname, const char *id)
{
	Eina_List *l;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("Package %s is not exists\n", pkgname);
		return NULL;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		if (!strcmp(instance_id(inst), id))
			return inst;
	}

	return NULL;
}

struct inst_info *package_find_instance_by_timestamp(const char *pkgname, double timestamp)
{
	Eina_List *l;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("Package %s is not exists\n", pkgname);
		return NULL;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		if (instance_timestamp(inst) == timestamp)
			return inst;
	}

	return NULL;
}

int package_dump_fault_info(struct pkg_info *info)
{
	if (!info->fault_info)
		return -ENOENT;

	ErrPrint("=============\n");
	ErrPrint("faulted at %lf\n", info->fault_info->timestamp);
	ErrPrint("Package: %s\n", info->pkgname);
	ErrPrint("Function: %s\n", info->fault_info->function);
	ErrPrint("InstanceID: %s\n", info->fault_info->filename);
	return 0;
}

int package_get_fault_info(struct pkg_info *info, double *timestamp, const char **filename, const char **function)
{
	if (!info->fault_info)
		return -ENOENT;

	*timestamp = info->fault_info->timestamp;
	*filename = info->fault_info->filename;
	*function = info->fault_info->function;
	return 0;
}

int package_set_fault_info(struct pkg_info *info, double timestamp, const char *filename, const char *function)
{
	struct fault_info *fault;

	package_clear_fault(info);

	fault = calloc(1, sizeof(*fault));
	if (!fault) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	fault->timestamp = timestamp;
	if (!filename)
		filename = "unknown";
	if (!function)
		function = "unknown";

	fault->filename = strdup(filename);
	if (!fault->filename) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(fault);
		return -ENOMEM;
	}

	fault->function = strdup(function);
	if (!fault->function) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(fault->filename);
		free(fault);
		return -ENOMEM;
	}

	info->fault_info = fault;
	return 0;
}

int package_clear_fault(struct pkg_info *info)
{
	if (!info->fault_info)
		return 0;
	
	package_dump_fault_info(info);

	free(info->fault_info->function);
	free(info->fault_info->filename);
	free(info->fault_info);
	info->fault_info = NULL;
	return 0;
}

int const package_is_fault(struct pkg_info *info)
{
	return !!info->fault_info;
}

struct slave_node * const package_slave(struct pkg_info *info)
{
	return info->slave;
}

int const package_timeout(struct pkg_info *info)
{
	return info->timeout;
}

double const package_period(struct pkg_info *info)
{
	return info->period;
}

int const package_secured(struct pkg_info *info)
{
	return info->secured;
}

const char * const package_script(struct pkg_info *info)
{
	return info->script;
}

const char * const package_abi(struct pkg_info *info)
{
	return info->abi;
}

const char * const package_lb_path(struct pkg_info *info)
{
	if (info->lb.type != LB_TYPE_SCRIPT)
		return NULL;

	return info->lb.info.script.path;
}

const char * const package_lb_group(struct pkg_info *info)
{
	if (info->lb.type != LB_TYPE_SCRIPT)
		return NULL;

	return info->lb.info.script.group;
}

const char * const package_pd_path(struct pkg_info *info)
{
	if (info->pd.type != PD_TYPE_SCRIPT)
		return NULL;

	return info->pd.info.script.path;
}

const char * const package_pd_group(struct pkg_info *info)
{
	if (info->pd.type != PD_TYPE_SCRIPT)
		return NULL;

	return info->pd.info.script.group;
}

int const package_pinup(struct pkg_info *info)
{
	return info->lb.pinup;
}

int const package_auto_launch(struct pkg_info *info)
{
	return info->lb.auto_launch;
}

unsigned int const package_size_list(struct pkg_info *info)
{
	return info->lb.size_list;
}

int const package_pd_width(struct pkg_info *info)
{
	return info->pd.width;
}

int const package_pd_height(struct pkg_info *info)
{
	return info->pd.height;
}

struct pkg_info * const package_ref(struct pkg_info *info)
{
	info->refcnt++;
	return info;
}

struct pkg_info * const package_unref(struct pkg_info *info)
{
	if (info->refcnt == 0) {
		ErrPrint("Invalid request\n");
		return NULL;
	}

	info->refcnt--;
	if (info->refcnt == 0) {
		destroy_package(info);
		info = NULL;
	}

	return info;
}

int const package_refcnt(struct pkg_info *info)
{
	return info->refcnt;
}

enum lb_type package_lb_type(struct pkg_info *info)
{
	return info->lb.type;
}

enum pd_type package_pd_type(struct pkg_info *info)
{
	return info->pd.type;
}

int package_add_instance(struct pkg_info *info, struct inst_info *inst)
{
	info->inst_list = eina_list_append(info->inst_list, inst);
	return 0;
}

int package_del_instance(struct pkg_info *info, struct inst_info *inst)
{
	info->inst_list = eina_list_remove(info->inst_list, inst);
	return 0;
}

Eina_List *package_instance_list(struct pkg_info *info)
{
	return info->inst_list;
}

static int client_created_cb(struct client_node *client, void *data)
{
	struct pkg_info *info;
	Eina_List *l;

	struct inst_info *inst;
	Eina_List *i_l;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->fault_info) {
			fault_unicast_info(client,
				info->pkgname, info->fault_info->filename, info->fault_info->function);
			continue;
		}

		EINA_LIST_FOREACH(info->inst_list, i_l, inst) {
			if (instance_state(inst) != INST_ACTIVATED) {
				DbgPrint("%s(%s) is not activated (%d)\n",
						package_name(info), instance_id(inst), instance_state(inst));
				continue;
			}

			instance_unicast_created_event(inst, client);
			DbgPrint("Created package: %s\n", info->pkgname);
		}
	}

	return 0;
}

int package_init(void)
{
	client_global_event_handler_add(CLIENT_GLOBAL_EVENT_CREATE, client_created_cb, NULL);
	return 0;
}

int package_fini(void)
{
	client_global_event_handler_del(CLIENT_GLOBAL_EVENT_CREATE, client_created_cb, NULL);
	return 0;
}

const char *package_find_by_secured_slave(struct slave_node *slave)
{
	Eina_List *l;
	struct pkg_info *info;

	if (!slave_is_secured(slave))
		return NULL;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave)
			return info->pkgname;
	}

	return NULL;
}

const char * const package_name(struct pkg_info *info)
{
	return info->pkgname;
}

/* End of a file */
