#include <stdio.h>
#include <errno.h>
#include <string.h> /* strcmp */
#include <stdlib.h> /* free */

#include <dlog.h>
#include <Eina.h>
#include <Ecore_Evas.h>

#include <packet.h>

#include "debug.h"
#include "util.h"
#include "parser.h"
#include "conf.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "package.h"
#include "fault_manager.h"
#include "instance.h"
#include "script_handler.h"
#include "group.h"
#include "abi.h"
#include "io.h"

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

			struct {
				/*!< Reserved for future use */
			} buffer;
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

			struct {
				/*!< Reserved for future use */
			} buffer;
		} info;

		unsigned int width;
		unsigned int height;
	} pd;

	int network;
	int timeout;
	double period;
	int secured;
	char *script; /* script type: edje, ... */
	char *abi;
	char *libexec;

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
	int ret;

	if (!slave_need_to_reactivate_instances(slave)) {
		DbgPrint("Do not need to reactivate instances\n");
		return 0;
	}

	cnt = 0;
	EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
		ret = instance_recover_state(inst);
		if (!ret)
			continue;

		instance_thaw_updator(inst);
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
			cnt += instance_need_slave(inst);
			/*!
			 * instance_deactivated will call the slave_unload_instance.
			 * if the loaded instance counter meets 0,
			 * the slave will be deactivated.
			 * so we should not call the instance activate function
			 * from here.
			 *
			 * activate slave when the slave is reactivated
			 */
		}
	}

	return cnt ? SLAVE_NEED_TO_REACTIVATE : 0;
}

static int slave_paused_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = (struct pkg_info *)data;
	struct inst_info *inst;
	Eina_List *l;

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		instance_freeze_updator(inst);
	}

	return 0;
}

static int slave_resume_cb(struct slave_node *slave, void *data)
{
	struct pkg_info *info = (struct pkg_info *)data;
	struct inst_info *inst;
	Eina_List *l;

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		instance_thaw_updator(inst);
	}

	return 0;
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
	free(info->libexec);

	free(info);
}

static inline int load_conf(struct pkg_info *info)
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
	} else if (parser_buffer_lb(parser)) {
		info->lb.type = LB_TYPE_BUFFER;
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
	} else if (parser_buffer_pd(parser)) {
		info->pd.type = PD_TYPE_BUFFER;
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
	info->network = parser_network(parser);

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
	if (group && group_add_livebox(group, info->pkgname) < 0)
		ErrPrint("Failed to build cluster tree for %s{%s}\n", info->pkgname, group);

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

	if (io_load_package_db(info) < 0) {
		ErrPrint("Failed to load DB, fall back to conf file loader\n");
	}

	if (load_conf(info) < 0) {
		ErrPrint("Failed to initiate the conf file loader\n");
		free(info->pkgname);
		free(info);
		return NULL;
	}

	package_ref(info);

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

	if (!pkgname)
		return NULL;

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
		return -EINVAL;
	
	package_dump_fault_info(info);

	free(info->fault_info->function);
	free(info->fault_info->filename);
	free(info->fault_info);
	info->fault_info = NULL;
	return 0;
}

const int const package_is_fault(const struct pkg_info *info)
{
	return !!info->fault_info;
}

struct slave_node * const package_slave(const struct pkg_info *info)
{
	return info->slave;
}

const int const package_timeout(const struct pkg_info *info)
{
	return info->timeout;
}

void package_set_timeout(struct pkg_info *info, int timeout)
{
	info->timeout = timeout;
}

const double const package_period(const struct pkg_info *info)
{
	return info->period;
}

void package_set_period(struct pkg_info *info, double period)
{
	info->period = period;
}

const int const package_secured(const struct pkg_info *info)
{
	return info->secured;
}

void package_set_secured(struct pkg_info *info, int secured)
{
	info->secured = secured;
}

const char * const package_script(const struct pkg_info *info)
{
	return info->script;
}

int package_set_script(struct pkg_info *info, const char *script)
{
	char *tmp;

	tmp = strdup(script);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->script);
	info->script = tmp;
	return 0;
}

const char * const package_abi(const struct pkg_info *info)
{
	return info->abi;
}

int package_set_abi(struct pkg_info *info, const char *abi)
{
	char *tmp;
	tmp = strdup(abi);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->abi);
	info->abi = tmp;
	return 0;
}

const char * const package_lb_path(const struct pkg_info *info)
{
	if (info->lb.type != LB_TYPE_SCRIPT)
		return NULL;

	return info->lb.info.script.path;
}

int package_set_lb_path(struct pkg_info *info, const char *path)
{
	char *tmp;

	if (info->lb.type != LB_TYPE_SCRIPT)
		return -EINVAL;

	tmp = strdup(path);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->lb.info.script.path);
	info->lb.info.script.path = tmp;
	return 0;
}

const char * const package_lb_group(const struct pkg_info *info)
{
	if (info->lb.type != LB_TYPE_SCRIPT)
		return NULL;

	return info->lb.info.script.group;
}

int package_set_lb_group(struct pkg_info *info, const char *group)
{
	char *tmp;

	if (info->lb.type != LB_TYPE_SCRIPT)
		return -EINVAL;

	tmp = strdup(group);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->lb.info.script.group);
	info->lb.info.script.group = tmp;
	return 0;
}

const char * const package_pd_path(const struct pkg_info *info)
{
	if (info->pd.type != PD_TYPE_SCRIPT)
		return NULL;

	return info->pd.info.script.path;
}

int package_set_pd_path(struct pkg_info *info, const char *path)
{
	char *tmp;

	if (info->pd.type != PD_TYPE_SCRIPT)
		return -EINVAL;

	tmp = strdup(path);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->pd.info.script.path);
	info->pd.info.script.path = tmp;
	return 0;
}

const char * const package_pd_group(const struct pkg_info *info)
{
	if (info->pd.type != PD_TYPE_SCRIPT)
		return NULL;

	return info->pd.info.script.group;
}

int package_set_pd_group(struct pkg_info *info, const char *group)
{
	char *tmp;

	if (info->pd.type != PD_TYPE_SCRIPT)
		return -EINVAL;

	tmp = strdup(group);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->pd.info.script.group);
	info->pd.info.script.group = tmp;
	return 0;
}

const int const package_pinup(const struct pkg_info *info)
{
	return info->lb.pinup;
}

void package_set_pinup(struct pkg_info *info, int pinup)
{
	info->lb.pinup = pinup;
}

const int const package_auto_launch(const struct pkg_info *info)
{
	return info->lb.auto_launch;
}

void package_set_auto_launch(struct pkg_info *info, int auto_launch)
{
	info->lb.auto_launch = auto_launch;
}

const unsigned int const package_size_list(const struct pkg_info *info)
{
	return info->lb.size_list;
}

void package_set_size_list(struct pkg_info *info, unsigned int size_list)
{
	info->lb.size_list = size_list;
}

const int const package_pd_width(const struct pkg_info *info)
{
	return info->pd.width;
}

void package_set_pd_width(struct pkg_info *info, int width)
{
	info->pd.width = width;
}

const int const package_pd_height(const struct pkg_info *info)
{
	return info->pd.height;
}

void package_set_pd_height(struct pkg_info *info, int height)
{
	info->pd.height = height;
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

const int const package_refcnt(const struct pkg_info *info)
{
	return info->refcnt;
}

const enum lb_type package_lb_type(const struct pkg_info *info)
{
	return info->lb.type;
}

void package_set_lb_type(struct pkg_info *info, enum lb_type type)
{
	info->lb.type = type;
}

const char * const package_libexec(struct pkg_info *info)
{
	return info->libexec;
}

int package_set_libexec(struct pkg_info *info, const char *libexec)
{
	char *tmp;

	tmp = strdup(libexec);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->libexec);
	info->libexec = tmp;
	return 0;
}

int package_network(struct pkg_info *info)
{
	return info->network;
}

void package_set_network(struct pkg_info *info, int network)
{
	info->network = network;
}

const enum pd_type const package_pd_type(const struct pkg_info *info)
{
	return info->pd.type;
}

void package_set_pd_type(struct pkg_info *info, enum pd_type type)
{
	info->pd.type = type;
}

int package_add_instance(struct pkg_info *info, struct inst_info *inst)
{
	if (!info->inst_list) {
		info->slave = slave_find_available(info->abi, info->secured);

		if (!info->slave) {
			char *s_name;
			char *s_pkgname;
			const char *tmp;

			s_name = util_slavename();
			if (!s_name) {
				ErrPrint("Failed to get a new slave name\n");
				return -EFAULT;
			}

			tmp = abi_find_slave(info->abi);
			if (!tmp) {
				free(s_name);
				ErrPrint("Failed to find a proper pkgname of a slave\n");
				return -EINVAL;
			}

			DbgPrint("Slave package: \"%s\" (abi: %s)\n", tmp, info->abi);
			s_pkgname = util_replace_string(tmp, REPLACE_TAG_APPID, info->pkgname);
			if (!s_pkgname) {
				s_pkgname = strdup(tmp);
				if (!s_pkgname) {
					ErrPrint("Heap: %s\n", strerror(errno));
					free(s_name);
					return -ENOMEM;
				}
			} else if (!info->secured) {
				DbgPrint("Slave package name is specified but the livebox is not secured\n");
				DbgPrint("Forcely set secured flag for livebox %s\n", info->pkgname);
				info->secured = 1;
			}

			DbgPrint("New slave name is %s, it is assigned for livebox %s (using %s)\n", s_name, info->pkgname, s_pkgname);
			info->slave = slave_create(s_name, info->secured, info->abi, s_pkgname);

			free(s_name);
			free(s_pkgname);

			if (info->slave) {
				slave_rpc_initialize(info->slave);
			} else {
				/*!
				 * \note
				 * package_destroy will try to remove "info" from the pkg_list.
				 * but we didn't add this to it yet.
				 * If the list method couldn't find an "info" from the list,
				 * it just do nothing so I'll leave this.
				 */
				return -EFAULT;
			}
			/*!
			 * \note
			 * Slave is not activated yet.
			 */
		} else {
			DbgPrint("Slave %s is assigned for %s\n", slave_name(info->slave), info->pkgname);
		}

		slave_load_package(info->slave);
		slave_event_callback_add(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
		slave_event_callback_add(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);

		if (info->secured) {
			slave_event_callback_add(info->slave, SLAVE_EVENT_PAUSE, slave_paused_cb, info);
			slave_event_callback_add(info->slave, SLAVE_EVENT_RESUME, slave_resume_cb, info);
		}
	}

	info->inst_list = eina_list_append(info->inst_list, inst);
	return 0;
}

int package_del_instance(struct pkg_info *info, struct inst_info *inst)
{
	info->inst_list = eina_list_remove(info->inst_list, inst);

	if (!info->inst_list) {
		if (info->slave) {
			slave_unload_package(info->slave);

			slave_event_callback_del(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
			slave_event_callback_del(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);

			if (info->secured) {
				slave_event_callback_del(info->slave, SLAVE_EVENT_PAUSE, slave_paused_cb, info);
				slave_event_callback_del(info->slave, SLAVE_EVENT_RESUME, slave_resume_cb, info);
			}

			info->slave = NULL;
		}
	}

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
			fault_unicast_info(client, info->pkgname, info->fault_info->filename, info->fault_info->function);
			continue;
		}

		EINA_LIST_FOREACH(info->inst_list, i_l, inst) {
			switch (instance_state(inst)) {
			case INST_INIT:
				/* Will be send a created event after the instance gets created event */
				break;
			case INST_ACTIVATED: /*!< This instance is actiavted, and used */
			case INST_REQUEST_TO_REACTIVATE: /*!< This instance will be reactivated soon */
			case INST_REQUEST_TO_DESTROY: /*!< This instance will be destroy soon */
				if (instance_client(inst) == client) {
					instance_unicast_created_event(inst, client);
				} else if (instance_client(inst) == NULL) {
					/*!
					 * \note
					 * Instances are lives in the system cluster/sub-cluster
					 */
					if (client_is_subscribed(client, instance_cluster(inst), instance_category(inst))) {
						instance_unicast_created_event(inst, client);
						DbgPrint("(Subscribed) Created package: %s\n", info->pkgname);
					}
				}

				break;
			default:
				DbgPrint("%s(%s) is not activated (%d)\n",
						package_name(info), instance_id(inst), instance_state(inst));
				break;
			}
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

const char * const package_name(const struct pkg_info *info)
{
	return info->pkgname;
}

int package_alter_instances_to_client(struct client_node *client)
{
	struct pkg_info *info;
	Eina_List *l;

	struct inst_info *inst;
	Eina_List *i_l;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		EINA_LIST_FOREACH(info->inst_list, i_l, inst) {
			switch (instance_state(inst)) {
			case INST_INIT:
				/* Will be send a created event after the instance gets created event */
				break;
			case INST_ACTIVATED: /*!< This instance is actiavted, and used */
			case INST_REQUEST_TO_REACTIVATE: /*!< This instance will be reactivated soon */
			case INST_REQUEST_TO_DESTROY: /*!< This instance will be destroy soon */
				if (instance_client(inst) == client) {
					instance_unicast_created_event(inst, client);
				} else if (instance_client(inst) == NULL) {
					/*!
					 * \note
					 * Instances are lives in the system cluster/sub-cluster
					 */
					if (client_is_subscribed(client, instance_cluster(inst), instance_category(inst))) {
						instance_unicast_created_event(inst, client);
						DbgPrint("(Subscribed) Created package: %s\n", info->pkgname);
					}
				}

				break;
			default:
				DbgPrint("%s(%s) is not activated (%d)\n",
						package_name(info), instance_id(inst), instance_state(inst));
				break;
			}
		}
	}

	return 0;
}

const Eina_List *package_list(void)
{
	return s_info.pkg_list;
}

int const package_fault_count(struct pkg_info *info)
{
	return info ? info->fault_count : 0;
}

/* End of a file */
