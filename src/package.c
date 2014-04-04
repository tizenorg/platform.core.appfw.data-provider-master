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
#include <errno.h>
#include <string.h> /* strcmp */
#include <stdlib.h> /* free */

#include <dlog.h>
#include <Eina.h>

#include <packet.h>
#include <livebox-errno.h>
#include <livebox-service.h>
#include <ail.h>

#include "critical_log.h"
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
#include "pkgmgr.h"
#include "xmonitor.h"

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
	char *pkgid;
	char *lbid;

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
		char *auto_launch;
		int pinup;
		int timeout;
		double period;
		char *libexec;
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
	int secured;
	char *script; /* script type: edje, ... */
	char *abi;

	int fault_count;
	struct fault_info *fault_info;

	struct slave_node *slave;
	int refcnt;

	Eina_List *inst_list;
	Eina_List *ctx_list;

	int is_uninstalled;
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
		if (!ret) {
			continue;
		}

		instance_thaw_updator(inst);
		cnt++;
	}

	DbgPrint("Recover state for %d instances of %s\n", cnt, package_name(info));
	return 0;
}

static int slave_fault_cb(struct slave_node *slave, void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct inst_info *inst;
	struct pkg_info *info = (struct pkg_info *)data;

	if (package_is_fault(info)) {
		ErrPrint("Already faulted package: %s\n", package_name(info));
		return 0;
	}

	(void)package_set_fault_info(info, util_timestamp(), slave_name(slave), __func__);
	fault_broadcast_info(package_name(info), slave_name(slave), __func__);

	DbgPrint("Slave critical fault - package: %s (by slave fault %s\n", package_name(info), slave_name(slave));
	EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
		DbgPrint("Destroy instance %p\n", inst);
		instance_destroyed(inst, LB_STATUS_ERROR_FAULT);
	}

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
			instance_destroyed(inst, LB_STATUS_ERROR_FAULT);
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

static int xmonitor_paused_cb(void *data)
{
	struct pkg_info *info = (struct pkg_info *)data;
	struct inst_info *inst;
	Eina_List *l;

	if (slave_state(info->slave) != SLAVE_TERMINATED) {
		return 0;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		instance_freeze_updator(inst);
	}

	return 0;
}

static int xmonitor_resumed_cb(void *data)
{
	struct pkg_info *info = data;
	struct inst_info *inst;
	Eina_List *l;

	if (slave_state(info->slave) != SLAVE_TERMINATED) {
		return 0;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		instance_thaw_updator(inst);
	}

	return 0;
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

static int slave_resumed_cb(struct slave_node *slave, void *data)
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
	eina_list_free(info->ctx_list);
	/* This items will be deleted from group_del_livebox */
	info->ctx_list = NULL;

	group_del_livebox(info->lbid);
	package_clear_fault(info);

	s_info.pkg_list = eina_list_remove(s_info.pkg_list, info);

	if (info->lb.type == LB_TYPE_SCRIPT) {
		DbgFree(info->lb.info.script.path);
		DbgFree(info->lb.info.script.group);
	}

	if (info->pd.type == PD_TYPE_SCRIPT) {
		DbgFree(info->pd.info.script.path);
		DbgFree(info->pd.info.script.group);
	}

	DbgFree(info->script);
	DbgFree(info->abi);
	DbgFree(info->lbid);
	DbgFree(info->lb.libexec);
	DbgFree(info->lb.auto_launch);
	DbgFree(info->pkgid);

	DbgFree(info);
}

static inline int load_conf(struct pkg_info *info)
{
	struct parser *parser;
	const char *str;
	const char *group;

	parser = parser_load(info->lbid);
	if (!parser) {
		info->lb.size_list = 0x01; /* Default */

		info->script = strdup(DEFAULT_SCRIPT);
		if (!info->script) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return LB_STATUS_ERROR_MEMORY;
		}

		info->abi = strdup(DEFAULT_ABI);
		if (!info->abi) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(info->script);
			info->script = NULL;
			return LB_STATUS_ERROR_MEMORY;
		}

		info->pd.width = g_conf.width;
		info->pd.height = g_conf.height >> 2;
		info->lb.pinup = 1;
		return LB_STATUS_SUCCESS;
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
				return LB_STATUS_ERROR_MEMORY;
			}

			str = parser_lb_group(parser);
			if (str) {
				info->lb.info.script.group = strdup(str);
				if (!info->lb.info.script.group) {
					ErrPrint("Heap: %s\n", strerror(errno));
					DbgFree(info->lb.info.script.path);
					parser_unload(parser);
					return LB_STATUS_ERROR_MEMORY;
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
					DbgFree(info->lb.info.script.path);
					DbgFree(info->lb.info.script.group);
				}
				parser_unload(parser);
				return LB_STATUS_ERROR_MEMORY;
			}

			str = parser_pd_group(parser);
			if (str) {
				info->pd.info.script.group = strdup(str);
				if (!info->pd.info.script.group) {
					ErrPrint("Heap: %s\n", strerror(errno));
					DbgFree(info->pd.info.script.path);
					if (info->lb.type == LB_TYPE_SCRIPT) {
						DbgFree(info->lb.info.script.path);
						DbgFree(info->lb.info.script.group);
					}
					parser_unload(parser);
					return LB_STATUS_ERROR_MEMORY;
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
			DbgFree(info->pd.info.script.path);
			DbgFree(info->pd.info.script.group);
		}

		if (info->lb.type == LB_TYPE_SCRIPT) {
			DbgFree(info->lb.info.script.path);
			DbgFree(info->lb.info.script.group);
		}

		parser_unload(parser);
		return LB_STATUS_ERROR_MEMORY;
	}

	str = parser_abi(parser);
	str = str ? str : DEFAULT_ABI;
	info->abi = strdup(str);
	if (!info->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(info->script);
		if (info->pd.type == PD_TYPE_SCRIPT) {
			DbgFree(info->pd.info.script.path);
			DbgFree(info->pd.info.script.group);
		}

		if (info->lb.type == LB_TYPE_SCRIPT) {
			DbgFree(info->lb.info.script.path);
			DbgFree(info->lb.info.script.group);
		}
		parser_unload(parser);
		return LB_STATUS_ERROR_MEMORY;
	}

	info->lb.timeout = parser_timeout(parser);
	info->network = parser_network(parser);

	info->lb.period = parser_period(parser);
	if (info->lb.period < 0.0f) {
		info->lb.period = 0.0f;
	} else if (info->lb.period > 0.0f && info->lb.period < MINIMUM_PERIOD) {
		info->lb.period = MINIMUM_PERIOD;
	}

	info->lb.size_list = parser_size(parser);

	str = parser_auto_launch(parser);
	str = str ? str : "";
	info->lb.auto_launch = strdup(str);
	if (!info->lb.auto_launch) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(info->abi);
		DbgFree(info->script);
		if (info->pd.type == PD_TYPE_SCRIPT) {
			DbgFree(info->pd.info.script.path);
			DbgFree(info->pd.info.script.group);
		}

		if (info->lb.type == LB_TYPE_SCRIPT) {
			DbgFree(info->lb.info.script.path);
			DbgFree(info->lb.info.script.group);
		}
		parser_unload(parser);
		return LB_STATUS_ERROR_MEMORY;
	}

	info->secured = parser_secured(parser);
	info->lb.pinup = parser_pinup(parser);

	parser_get_pdsize(parser, &info->pd.width, &info->pd.height);

	group = parser_group_str(parser);
	if (group && group_add_livebox(group, info->lbid) < 0) {
		ErrPrint("Failed to build cluster tree for %s{%s}\n", info->lbid, group);
	}

	parser_unload(parser);
	return LB_STATUS_SUCCESS;
}

HAPI struct pkg_info *package_create(const char *pkgid, const char *lbid)
{
	struct pkg_info *pkginfo;

	pkginfo = calloc(1, sizeof(*pkginfo));
	if (!pkginfo) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	pkginfo->pkgid = strdup(pkgid);
	if (!pkginfo->pkgid) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(pkginfo);
		return NULL;
	}

	pkginfo->lbid = io_livebox_pkgname(lbid);
	if (!pkginfo->lbid) {
		ErrPrint("Failed to get pkgname, fallback to fs checker\n");
		if (util_validate_livebox_package(lbid) < 0) {
			ErrPrint("Invalid package name: %s\n", lbid);
			DbgFree(pkginfo->pkgid);
			DbgFree(pkginfo);
			return NULL;
		}

		pkginfo->lbid = strdup(lbid);
		if (!pkginfo->lbid) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(pkginfo->pkgid);
			DbgFree(pkginfo);
			return NULL;
		}
	}

	if (io_load_package_db(pkginfo) < 0) {
		ErrPrint("Failed to load DB, fall back to conf file loader\n");
		if (load_conf(pkginfo) < 0) {
			ErrPrint("Failed to initiate the conf file loader\n");
			DbgFree(pkginfo->lbid);
			DbgFree(pkginfo->pkgid);
			DbgFree(pkginfo);
			return NULL;
		}
	}

	package_ref(pkginfo);

	s_info.pkg_list = eina_list_append(s_info.pkg_list, pkginfo);

	return pkginfo;
}

HAPI int package_destroy(struct pkg_info *info)
{
	package_unref(info);
	return LB_STATUS_SUCCESS;
}

HAPI Eina_List *package_ctx_info(struct pkg_info *pkginfo)
{
	return pkginfo->ctx_list;
}

HAPI void package_add_ctx_info(struct pkg_info *pkginfo, struct context_info *info)
{
	pkginfo->ctx_list = eina_list_append(pkginfo->ctx_list, info);
}

HAPI void package_del_ctx_info(struct pkg_info *pkginfo, struct context_info *info)
{
	pkginfo->ctx_list = eina_list_remove(pkginfo->ctx_list, info);
}

HAPI char *package_lb_pkgname(const char *pkgname)
{
	char *lbid;

	lbid = io_livebox_pkgname(pkgname);
	if (!lbid) {
		if (util_validate_livebox_package(pkgname) < 0) {
			return NULL;
		}

		lbid = strdup(pkgname);
		if (!lbid) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return NULL;
		}
	}

	return lbid;
}

HAPI int package_is_lb_pkgname(const char *pkgname)
{
	char *lbid;
	int ret;

	lbid = package_lb_pkgname(pkgname);
	ret = !!lbid;
	DbgFree(lbid);

	return ret;
}

HAPI struct pkg_info *package_find(const char *lbid)
{
	Eina_List *l;
	struct pkg_info *info;

	if (!lbid) {
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (!strcmp(info->lbid, lbid)) {
			return info;
		}
	}

	return NULL;
}

HAPI struct inst_info *package_find_instance_by_id(const char *lbid, const char *id)
{
	Eina_List *l;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(lbid);
	if (!info) {
		ErrPrint("Package %s is not exists\n", lbid);
		return NULL;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		if (!strcmp(instance_id(inst), id)) {
			return inst;
		}
	}

	return NULL;
}

HAPI struct inst_info *package_find_instance_by_timestamp(const char *lbid, double timestamp)
{
	Eina_List *l;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(lbid);
	if (!info) {
		ErrPrint("Package %s is not exists\n", lbid);
		return NULL;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		if (instance_timestamp(inst) == timestamp) {
			return inst;
		}
	}

	return NULL;
}

HAPI int package_dump_fault_info(struct pkg_info *info)
{
	if (!info->fault_info) {
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	CRITICAL_LOG("=============\n");
	CRITICAL_LOG("faulted at %lf\n", info->fault_info->timestamp);
	CRITICAL_LOG("Package: %s\n", info->lbid);
	CRITICAL_LOG("Function: %s\n", info->fault_info->function);
	CRITICAL_LOG("InstanceID: %s\n", info->fault_info->filename);
	return LB_STATUS_SUCCESS;
}

HAPI int package_get_fault_info(struct pkg_info *info, double *timestamp, const char **filename, const char **function)
{
	if (!info->fault_info) {
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	*timestamp = info->fault_info->timestamp;
	*filename = info->fault_info->filename;
	*function = info->fault_info->function;
	return LB_STATUS_SUCCESS;
}

HAPI int package_set_fault_info(struct pkg_info *info, double timestamp, const char *filename, const char *function)
{
	struct fault_info *fault;

	package_clear_fault(info);

	fault = calloc(1, sizeof(*fault));
	if (!fault) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	fault->timestamp = timestamp;
	if (!filename) {
		filename = "unknown";
	}
	if (!function) {
		function = "unknown";
	}

	fault->filename = strdup(filename);
	if (!fault->filename) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(fault);
		return LB_STATUS_ERROR_MEMORY;
	}

	fault->function = strdup(function);
	if (!fault->function) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(fault->filename);
		DbgFree(fault);
		return LB_STATUS_ERROR_MEMORY;
	}

	info->fault_info = fault;
	info->fault_count++;
	return LB_STATUS_SUCCESS;
}

HAPI int package_clear_fault(struct pkg_info *info)
{
	if (!info->fault_info) {
		return LB_STATUS_ERROR_INVALID;
	}
	
	package_dump_fault_info(info);

	DbgFree(info->fault_info->function);
	DbgFree(info->fault_info->filename);
	DbgFree(info->fault_info);
	info->fault_info = NULL;
	return LB_STATUS_SUCCESS;
}

HAPI const int const package_is_fault(const struct pkg_info *info)
{
	return !!info->fault_info;
}

HAPI struct slave_node * const package_slave(const struct pkg_info *info)
{
	return info->slave;
}

HAPI const int const package_timeout(const struct pkg_info *info)
{
	return info->lb.timeout;
}

HAPI void package_set_timeout(struct pkg_info *info, int timeout)
{
	info->lb.timeout = timeout;
}

HAPI const double const package_period(const struct pkg_info *info)
{
	return info->lb.period;
}

HAPI void package_set_period(struct pkg_info *info, double period)
{
	info->lb.period = period;
}

HAPI const int const package_secured(const struct pkg_info *info)
{
	return info->secured;
}

HAPI void package_set_secured(struct pkg_info *info, int secured)
{
	info->secured = secured;
}

HAPI const char * const package_script(const struct pkg_info *info)
{
	return info->script;
}

HAPI int package_set_script(struct pkg_info *info, const char *script)
{
	char *tmp;

	tmp = strdup(script);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->script);
	info->script = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI const char * const package_abi(const struct pkg_info *info)
{
	return info->abi;
}

HAPI int package_set_abi(struct pkg_info *info, const char *abi)
{
	char *tmp;
	tmp = strdup(abi);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->abi);
	info->abi = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI const char * const package_lb_path(const struct pkg_info *info)
{
	if (info->lb.type != LB_TYPE_SCRIPT) {
		return NULL;
	}

	return info->lb.info.script.path;
}

HAPI int package_set_lb_path(struct pkg_info *info, const char *path)
{
	char *tmp;

	if (info->lb.type != LB_TYPE_SCRIPT) {
		return LB_STATUS_ERROR_INVALID;
	}

	tmp = strdup(path);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->lb.info.script.path);
	info->lb.info.script.path = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI const char * const package_lb_group(const struct pkg_info *info)
{
	if (info->lb.type != LB_TYPE_SCRIPT) {
		return NULL;
	}

	return info->lb.info.script.group;
}

HAPI int package_set_lb_group(struct pkg_info *info, const char *group)
{
	char *tmp;

	if (info->lb.type != LB_TYPE_SCRIPT) {
		return LB_STATUS_ERROR_INVALID;
	}

	tmp = strdup(group);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->lb.info.script.group);
	info->lb.info.script.group = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI const char * const package_pd_path(const struct pkg_info *info)
{
	if (info->pd.type != PD_TYPE_SCRIPT) {
		return NULL;
	}

	return info->pd.info.script.path;
}

HAPI int package_set_pd_path(struct pkg_info *info, const char *path)
{
	char *tmp;

	if (info->pd.type != PD_TYPE_SCRIPT) {
		return LB_STATUS_ERROR_INVALID;
	}

	tmp = strdup(path);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->pd.info.script.path);
	info->pd.info.script.path = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI const char * const package_pd_group(const struct pkg_info *info)
{
	if (info->pd.type != PD_TYPE_SCRIPT) {
		return NULL;
	}

	return info->pd.info.script.group;
}

HAPI int package_set_pd_group(struct pkg_info *info, const char *group)
{
	char *tmp;

	if (info->pd.type != PD_TYPE_SCRIPT) {
		return LB_STATUS_ERROR_INVALID;
	}

	tmp = strdup(group);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->pd.info.script.group);
	info->pd.info.script.group = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI const int const package_pinup(const struct pkg_info *info)
{
	return info->lb.pinup;
}

HAPI void package_set_pinup(struct pkg_info *info, int pinup)
{
	info->lb.pinup = pinup;
}

HAPI const char * const package_auto_launch(const struct pkg_info *info)
{
	return info->lb.auto_launch;
}

HAPI void package_set_auto_launch(struct pkg_info *info, const char *auto_launch)
{
	if (!auto_launch) {
		auto_launch = "";
	}

	info->lb.auto_launch = strdup(auto_launch);
	if (!info->lb.auto_launch) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return;
	}
}

HAPI const unsigned int const package_size_list(const struct pkg_info *info)
{
	return info->lb.size_list;
}

HAPI void package_set_size_list(struct pkg_info *info, unsigned int size_list)
{
	info->lb.size_list = size_list;
}

HAPI const int const package_pd_width(const struct pkg_info *info)
{
	return info->pd.width;
}

HAPI void package_set_pd_width(struct pkg_info *info, int width)
{
	info->pd.width = width;
}

HAPI const int const package_pd_height(const struct pkg_info *info)
{
	return info->pd.height;
}

HAPI void package_set_pd_height(struct pkg_info *info, int height)
{
	info->pd.height = height;
}

HAPI struct pkg_info * const package_ref(struct pkg_info *info)
{
	info->refcnt++;
	return info;
}

HAPI struct pkg_info * const package_unref(struct pkg_info *info)
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

HAPI const int const package_refcnt(const struct pkg_info *info)
{
	return info->refcnt;
}

HAPI const enum lb_type package_lb_type(const struct pkg_info *info)
{
	return info ? info->lb.type : LB_TYPE_NONE;
}

HAPI void package_set_lb_type(struct pkg_info *info, enum lb_type type)
{
	info->lb.type = type;
}

HAPI const char * const package_libexec(struct pkg_info *info)
{
	return info->lb.libexec;
}

HAPI int package_set_libexec(struct pkg_info *info, const char *libexec)
{
	char *tmp;

	tmp = strdup(libexec);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	DbgFree(info->lb.libexec);
	info->lb.libexec = tmp;
	return LB_STATUS_SUCCESS;
}

HAPI int package_network(struct pkg_info *info)
{
	return info->network;
}

HAPI void package_set_network(struct pkg_info *info, int network)
{
	info->network = network;
}

HAPI const enum pd_type const package_pd_type(const struct pkg_info *info)
{
	return info ? info->pd.type : PD_TYPE_NONE;
}

HAPI void package_set_pd_type(struct pkg_info *info, enum pd_type type)
{
	info->pd.type = type;
}

/*!
 * \note
 * Add the instance to the package info.
 * If a package has no slave, assign a new slave.
 */
static inline int assign_new_slave(struct pkg_info *info)
{
	char *s_name;
	char *s_pkgname;
	const char *tmp;

	s_name = util_slavename();
	if (!s_name) {
		ErrPrint("Failed to get a new slave name\n");
		return LB_STATUS_ERROR_FAULT;
	}

	tmp = abi_find_slave(info->abi);
	if (!tmp) {
		DbgFree(s_name);
		ErrPrint("Failed to find a proper pkgname of a slave\n");
		return LB_STATUS_ERROR_INVALID;
	}

	s_pkgname = util_replace_string(tmp, REPLACE_TAG_APPID, info->lbid);
	if (!s_pkgname) {
		DbgPrint("Failed to get replaced string\n");
		s_pkgname = strdup(tmp);
		if (!s_pkgname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(s_name);
			return LB_STATUS_ERROR_MEMORY;
		}
	}

	DbgPrint("New slave[%s] is assigned for %s (using %s / abi[%s])\n", s_name, info->lbid, s_pkgname, info->abi);
	info->slave = slave_create(s_name, info->secured, info->abi, s_pkgname, info->network);

	DbgFree(s_name);
	DbgFree(s_pkgname);

	if (!info->slave) {
		/*!
		 * \note
		 * package_destroy will try to remove "info" from the pkg_list.
		 * but we didn't add this to it yet.
		 * If the list method couldn't find an "info" from the list,
		 * it just do nothing so I'll leave this.
		 */
		return LB_STATUS_ERROR_FAULT;
	}
	/*!
	 * \note
	 * Slave is not activated yet.
	 */
	return LB_STATUS_SUCCESS;
}

HAPI int package_add_instance(struct pkg_info *info, struct inst_info *inst)
{
	if (!info->inst_list) {
		info->slave = slave_find_available(info->abi, info->secured, info->network);

		if (!info->slave) {
			int ret;

			ret = assign_new_slave(info);
			if (ret < 0) {
				return ret;
			}
		} else {
			DbgPrint("Slave %s is used for %s\n", slave_name(info->slave), info->lbid);
		}

		(void)slave_ref(info->slave);
		slave_load_package(info->slave);
		(void)slave_event_callback_add(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
		(void)slave_event_callback_add(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);
		(void)slave_event_callback_add(info->slave, SLAVE_EVENT_FAULT, slave_fault_cb, info);

		if (info->secured) {
			(void)slave_event_callback_add(info->slave, SLAVE_EVENT_PAUSE, slave_paused_cb, info);
			(void)slave_event_callback_add(info->slave, SLAVE_EVENT_RESUME, slave_resumed_cb, info);

			/*!
			 * \note
			 * In case of the slave is terminated because of expired TTL timer,
			 * Master should freeze the all update time.
			 * But the callback should check the slave's state to prevent from duplicated freezing.
			 *
			 * This callback will freeze the timer only if a slave doesn't running.
			 */
			(void)xmonitor_add_event_callback(XMONITOR_PAUSED, xmonitor_paused_cb, info);
			(void)xmonitor_add_event_callback(XMONITOR_RESUMED, xmonitor_resumed_cb, info);
		}
	}

	info->inst_list = eina_list_append(info->inst_list, inst);
	return LB_STATUS_SUCCESS;
}

HAPI int package_del_instance(struct pkg_info *info, struct inst_info *inst)
{
	info->inst_list = eina_list_remove(info->inst_list, inst);

	if (info->inst_list) {
		return LB_STATUS_SUCCESS;
	}

	if (info->slave) {
		slave_unload_package(info->slave);

		slave_event_callback_del(info->slave, SLAVE_EVENT_FAULT, slave_fault_cb, info);
		slave_event_callback_del(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
		slave_event_callback_del(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);

		if (info->secured) {
			slave_event_callback_del(info->slave, SLAVE_EVENT_PAUSE, slave_paused_cb, info);
			slave_event_callback_del(info->slave, SLAVE_EVENT_RESUME, slave_resumed_cb, info);

			xmonitor_del_event_callback(XMONITOR_PAUSED, xmonitor_paused_cb, info);
			xmonitor_del_event_callback(XMONITOR_RESUMED, xmonitor_resumed_cb, info);
		}

		slave_unref(info->slave);
		info->slave = NULL;
	}

	if (info->is_uninstalled) {
		package_destroy(info);
	}

	return LB_STATUS_SUCCESS;
}

HAPI Eina_List *package_instance_list(struct pkg_info *info)
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
			fault_unicast_info(client, info->lbid, info->fault_info->filename, info->fault_info->function);
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
						DbgPrint("(Subscribed) Created package: %s\n", info->lbid);
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

static int io_uninstall_cb(const char *pkgid, const char *lbid, int prime, void *data)
{
	struct pkg_info *info;
	Eina_List *l;
	Eina_List *n;
	struct inst_info *inst;

	DbgPrint("Package %s is uninstalled\n", lbid);
	info = package_find(lbid);
	if (!info) {
		DbgPrint("%s is not yet loaded\n", lbid);
		return 0;
	}

	info->is_uninstalled = 1;

	/*!
	 * \NOTE
	 * Don't delete an item from the inst_list.
	 * destroy callback will use this list again.
	 * So, Don't touch it from here.
	 */
	if (info->inst_list) {
		EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
			instance_destroy(inst, INSTANCE_DESTROY_UNINSTALL);
		}
	} else {
		package_destroy(info);
	}

	return 0;
}

static inline void reload_package_info(struct pkg_info *info)
{
	Eina_List *l;
	Eina_List *n;
	struct inst_info *inst;
	unsigned int size_type;
	int width;
	int height;
	double old_period;

	DbgPrint("Already exists, try to update it\n");

	old_period = info->lb.period;

	group_del_livebox(info->lbid);
	package_clear_fault(info);

	/*!
	 * \NOTE:
	 * Nested DB I/O
	 */
	io_load_package_db(info);

	/*!
	 * \note
	 * Without "is_uninstalled", the package will be kept
	 */
	EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
		width = instance_lb_width(inst);
		height = instance_lb_height(inst);
		size_type = livebox_service_size_type(width, height);
		if (info->lb.size_list & size_type) {
			if (instance_period(inst) == old_period) {
				instance_reload_period(inst, package_period(info));
			}
			instance_reload(inst, INSTANCE_DESTROY_UPGRADE);
		} else {
			instance_destroy(inst, INSTANCE_DESTROY_UNINSTALL);
		}
	}
}

static int io_install_cb(const char *pkgid, const char *lbid, int prime, void *data)
{
	struct pkg_info *info;

	info = package_find(lbid);
	if (info) {
		/*!
		 * Already exists. skip to create this.
		 */
		return 0;
	}

	info = package_create(pkgid, lbid);
	if (!info) {
		ErrPrint("Failed to build an info %s\n", lbid);
	} else {
		DbgPrint("Livebox %s is built\n", lbid);
	}

	return 0;
}

static int uninstall_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct pkg_info *info;

	if (status != PKGMGR_STATUS_END) {
		return 0;
	}

	EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, n, info) {
		if (!strcmp(info->pkgid, pkgname)) {
			io_uninstall_cb(pkgname, info->lbid, -1, NULL);
		}
	}

	return 0;
}

static int update_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct pkg_info *info;

	if (status != PKGMGR_STATUS_END) {
		return 0;
	}

	EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, n, info) {
		if (!strcmp(info->pkgid, pkgname)) {
			DbgPrint("Update lbid: %s\n", info->lbid);
			if (io_is_exists(info->lbid) == 1) {
				reload_package_info(info);
			} else {
				io_uninstall_cb(pkgname, info->lbid, -1, NULL);
			}
		}
	}

	(void)io_update_livebox_package(pkgname, io_install_cb, NULL);
	return 0;
}

static int crawling_liveboxes(const char *pkgid, const char *lbid, int prime, void *data)
{
	if (package_find(lbid)) {
		ErrPrint("Information of %s is already built\n", lbid);
	} else {
		struct pkg_info *info;
		info = package_create(pkgid, lbid);
		if (info) {
			DbgPrint("[%s] information is built prime(%d)\n", lbid, prime);
		}
	}

	return 0;
}

HAPI int package_init(void)
{
	client_global_event_handler_add(CLIENT_GLOBAL_EVENT_CREATE, client_created_cb, NULL);
	pkgmgr_init();

	pkgmgr_add_event_callback(PKGMGR_EVENT_INSTALL, update_cb, NULL);
	pkgmgr_add_event_callback(PKGMGR_EVENT_UNINSTALL, uninstall_cb, NULL);
	pkgmgr_add_event_callback(PKGMGR_EVENT_UPDATE, update_cb, NULL);

	io_crawling_liveboxes(crawling_liveboxes, NULL);
	return 0;
}

HAPI int package_fini(void)
{
	Eina_List *p_l;
	Eina_List *p_n;
	Eina_List *i_l;
	Eina_List *i_n;
	struct pkg_info *info;
	struct inst_info *inst;

	pkgmgr_del_event_callback(PKGMGR_EVENT_INSTALL, update_cb, NULL);
	pkgmgr_del_event_callback(PKGMGR_EVENT_UNINSTALL, uninstall_cb, NULL);
	pkgmgr_del_event_callback(PKGMGR_EVENT_UPDATE, update_cb, NULL);
	pkgmgr_fini();
	client_global_event_handler_del(CLIENT_GLOBAL_EVENT_CREATE, client_created_cb, NULL);

	EINA_LIST_FOREACH_SAFE(s_info.pkg_list, p_l, p_n, info) {
		EINA_LIST_FOREACH_SAFE(info->inst_list, i_l, i_n, inst) {
			instance_state_reset(inst);
			instance_destroy(inst, INSTANCE_DESTROY_TERMINATE);
		}

		package_destroy(info);
	}

	return 0;
}

HAPI const char *package_find_by_secured_slave(struct slave_node *slave)
{
	Eina_List *l;
	struct pkg_info *info;

	if (!slave_is_secured(slave)) {
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave) {
			return info->lbid;
		}
	}

	return NULL;
}

HAPI const char * const package_name(const struct pkg_info *info)
{
	return info->lbid;
}

/*!
 * del_or_creat : 1 == create, 0 == delete
 */
HAPI int package_alter_instances_to_client(struct client_node *client, enum alter_type alter)
{
	struct pkg_info *info;
	Eina_List *l;

	struct inst_info *inst;
	Eina_List *i_l;

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		EINA_LIST_FOREACH(info->inst_list, i_l, inst) {
			if (instance_client(inst)) {
				continue;
			}

			if (!client_is_subscribed(client, instance_cluster(inst), instance_category(inst))) {
				continue;
			}

			switch (instance_state(inst)) {
			case INST_INIT:
			case INST_REQUEST_TO_ACTIVATE:
				/* Will be send a created event after the instance gets created event */
				switch (alter) {
				case ALTER_CREATE:
					if (!instance_has_client(inst, client)) {
						instance_add_client(inst, client);
					}
					break;
				case ALTER_DESTROY:
					if (instance_has_client(inst, client)) {
						instance_del_client(inst, client);
					}
					break;
				default:
					break;
				}
				break;
			case INST_ACTIVATED: /*!< This instance is actiavted, and used */
			case INST_REQUEST_TO_REACTIVATE: /*!< This instance will be reactivated soon */
			case INST_REQUEST_TO_DESTROY: /*!< This instance will be destroy soon */
				/*!
				 * \note
				 * Instances are lives in the system cluster/sub-cluster
				 */
				switch (alter) {
				case ALTER_CREATE:
					if (!instance_has_client(inst, client)) {
						instance_unicast_created_event(inst, client);
						instance_add_client(inst, client);
						DbgPrint("(Subscribed) Created package: %s\n", info->lbid);
					}
					break;
				case ALTER_DESTROY:
					if (instance_has_client(inst, client)) {
						instance_unicast_deleted_event(inst, client, LB_STATUS_SUCCESS);
						instance_del_client(inst, client);
					}
					break;
				default:
					break;
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

HAPI const Eina_List *package_list(void)
{
	return s_info.pkg_list;
}

HAPI int const package_fault_count(struct pkg_info *info)
{
	return info ? info->fault_count : 0;
}

HAPI int package_is_enabled(const char *appid)
{
	ail_appinfo_h ai;
	bool enabled;
	int ret;

	ret = ail_get_appinfo(appid, &ai);
	if (ret != AIL_ERROR_OK) {
		ErrPrint("Unable to get appinfo: %d\n", ret);
		return 0;
	}

	if (ail_appinfo_get_bool(ai, AIL_PROP_X_SLP_ENABLED_BOOL, &enabled) != AIL_ERROR_OK) {
		enabled = false;
	}

	ail_destroy_appinfo(ai);

	return enabled == true;
}

HAPI int package_faulted(struct pkg_info *pkg, int broadcast)
{
	Eina_List *l;
	Eina_List *n;
	struct slave_node *slave;
	struct inst_info *inst;

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package has no slave?\n");
		return LB_STATUS_ERROR_FAULT;
	}

	/* Emulated fault routine */
	// (void)package_set_fault_info(pkg, util_timestamp(), slave_name(slave), __func__);
	if (broadcast) {
		fault_broadcast_info(package_name(pkg), slave_name(slave), __func__);
	}

	DbgPrint("package: %s (forucely faulted %s)\n", package_name(pkg), slave_name(slave));
	EINA_LIST_FOREACH_SAFE(pkg->inst_list, l, n, inst) {
		DbgPrint("Destroy instance %p\n", inst);
		instance_destroy(inst, INSTANCE_DESTROY_FAULT);
	}

	return LB_STATUS_SUCCESS;
}

/* End of a file */
