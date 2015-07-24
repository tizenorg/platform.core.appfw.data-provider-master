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
#include <widget_errno.h>
#include <widget_service.h>
#include <widget_service_internal.h>
#include <widget_conf.h>
#include <pkgmgr-info.h>

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
	char *widget_id;

	struct {
		enum widget_widget_type type;

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
	} widget;

	struct {
		enum widget_gbar_type type;

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
	} gbar;

	int network;
	int secured;
	int direct_input;
	char *script; /* script type: edje, ... */
	char *abi;
	char *hw_acceleration;
	char *category;

	double faulted_at; /* Fault started time */
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
	if (slave_is_watch(slave)) {
		EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
			/**
			 * @note
			 * Watch will be recovered by SLAVE_SYNC_HELLO command.
			 * Not from here.
			 */
			instance_watch_set_need_to_recover(inst, EINA_TRUE);
			cnt++;
		}
		DbgPrint("Watch instance: %d\n", cnt);
	} else {
		EINA_LIST_FOREACH_SAFE(info->inst_list, l, n, inst) {
			ret = instance_recover_state(inst);
			if (!ret) {
				continue;
			}

			instance_thaw_updator(inst);
			cnt++;
		}
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
		instance_destroyed(inst, WIDGET_ERROR_FAULT);
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
			instance_destroyed(inst, WIDGET_ERROR_FAULT);
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
		(void)instance_freeze_updator(inst);
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
		(void)instance_freeze_updator(inst);
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
	/* This items will be deleted from group_del_widget */
	info->ctx_list = NULL;

	group_del_widget(info->widget_id);
	package_clear_fault(info);

	s_info.pkg_list = eina_list_remove(s_info.pkg_list, info);

	if (info->widget.type == WIDGET_TYPE_SCRIPT) {
		DbgFree(info->widget.info.script.path);
		DbgFree(info->widget.info.script.group);
	}

	if (info->gbar.type == GBAR_TYPE_SCRIPT) {
		DbgFree(info->gbar.info.script.path);
		DbgFree(info->gbar.info.script.group);
	}

	DbgFree(info->script);
	DbgFree(info->abi);
	DbgFree(info->widget_id);
	DbgFree(info->widget.libexec);
	DbgFree(info->widget.auto_launch);
	DbgFree(info->pkgid);
	DbgFree(info->hw_acceleration);

	DbgFree(info);
}

static inline int load_conf(struct pkg_info *info)
{
	struct parser *parser;
	const char *str;
	const char *group;

	parser = parser_load(info->widget_id);
	if (!parser) {
		info->widget.size_list = 0x01; /* Default */

		info->script = strdup(WIDGET_CONF_DEFAULT_SCRIPT);
		if (!info->script) {
			ErrPrint("strdup: %d\n", errno);
			return WIDGET_ERROR_OUT_OF_MEMORY;
		}

		info->abi = strdup(WIDGET_CONF_DEFAULT_ABI);
		if (!info->abi) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(info->script);
			info->script = NULL;
			return WIDGET_ERROR_OUT_OF_MEMORY;
		}

		info->gbar.width = WIDGET_CONF_BASE_W;
		info->gbar.height = WIDGET_CONF_BASE_H >> 2;
		info->widget.pinup = 1;
		return WIDGET_ERROR_NONE;
	}

	info->widget.type = WIDGET_TYPE_FILE;
	if (parser_text_widget(parser)) {
		info->widget.type = WIDGET_TYPE_TEXT;
	} else if (parser_buffer_widget(parser)) {
		info->widget.type = WIDGET_TYPE_BUFFER;
	} else {
		str = parser_widget_path(parser);
		if (str) {
			info->widget.type = WIDGET_TYPE_SCRIPT;

			info->widget.info.script.path = strdup(str);
			if (!info->widget.info.script.path) {
				ErrPrint("strdup: %d\n", errno);
				parser_unload(parser);
				return WIDGET_ERROR_OUT_OF_MEMORY;
			}

			str = parser_widget_group(parser);
			if (str) {
				info->widget.info.script.group = strdup(str);
				if (!info->widget.info.script.group) {
					ErrPrint("strdup: %d\n", errno);
					DbgFree(info->widget.info.script.path);
					parser_unload(parser);
					return WIDGET_ERROR_OUT_OF_MEMORY;
				}
			}
		}
	}

	if (parser_text_gbar(parser)) {
		info->gbar.type = GBAR_TYPE_TEXT;
	} else if (parser_buffer_gbar(parser)) {
		info->gbar.type = GBAR_TYPE_BUFFER;
	} else {
		str = parser_gbar_path(parser);
		if (str) {
			info->gbar.type = GBAR_TYPE_SCRIPT;
			info->gbar.info.script.path = strdup(str);
			if (!info->gbar.info.script.path) {
				ErrPrint("strdup: %d\n", errno);
				if (info->widget.type == WIDGET_TYPE_SCRIPT) {
					DbgFree(info->widget.info.script.path);
					DbgFree(info->widget.info.script.group);
				}
				parser_unload(parser);
				return WIDGET_ERROR_OUT_OF_MEMORY;
			}

			str = parser_gbar_group(parser);
			if (str) {
				info->gbar.info.script.group = strdup(str);
				if (!info->gbar.info.script.group) {
					ErrPrint("strdup: %d\n", errno);
					DbgFree(info->gbar.info.script.path);
					if (info->widget.type == WIDGET_TYPE_SCRIPT) {
						DbgFree(info->widget.info.script.path);
						DbgFree(info->widget.info.script.group);
					}
					parser_unload(parser);
					return WIDGET_ERROR_OUT_OF_MEMORY;
				}
			}
		}
	}

	str = parser_script(parser);
	str = str ? str : WIDGET_CONF_DEFAULT_SCRIPT;
	info->script = strdup(str);
	if (!info->script) {
		ErrPrint("strdup: %d\n", errno);
		if (info->gbar.type == GBAR_TYPE_SCRIPT) {
			DbgFree(info->gbar.info.script.path);
			DbgFree(info->gbar.info.script.group);
		}

		if (info->widget.type == WIDGET_TYPE_SCRIPT) {
			DbgFree(info->widget.info.script.path);
			DbgFree(info->widget.info.script.group);
		}

		parser_unload(parser);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	str = parser_abi(parser);
	str = str ? str : WIDGET_CONF_DEFAULT_ABI;
	info->abi = strdup(str);
	if (!info->abi) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(info->script);
		if (info->gbar.type == GBAR_TYPE_SCRIPT) {
			DbgFree(info->gbar.info.script.path);
			DbgFree(info->gbar.info.script.group);
		}

		if (info->widget.type == WIDGET_TYPE_SCRIPT) {
			DbgFree(info->widget.info.script.path);
			DbgFree(info->widget.info.script.group);
		}
		parser_unload(parser);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	info->widget.timeout = parser_timeout(parser);
	info->network = parser_network(parser);

	info->widget.period = parser_period(parser);
	if (info->widget.period < 0.0f) {
		info->widget.period = 0.0f;
	} else if (info->widget.period > 0.0f && info->widget.period < WIDGET_CONF_MINIMUM_PERIOD) {
		info->widget.period = WIDGET_CONF_MINIMUM_PERIOD;
	}

	info->widget.size_list = parser_size(parser);

	str = parser_auto_launch(parser);
	str = str ? str : "";
	info->widget.auto_launch = strdup(str);
	if (!info->widget.auto_launch) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(info->abi);
		DbgFree(info->script);
		if (info->gbar.type == GBAR_TYPE_SCRIPT) {
			DbgFree(info->gbar.info.script.path);
			DbgFree(info->gbar.info.script.group);
		}

		if (info->widget.type == WIDGET_TYPE_SCRIPT) {
			DbgFree(info->widget.info.script.path);
			DbgFree(info->widget.info.script.group);
		}
		parser_unload(parser);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	info->secured = parser_secured(parser);
	info->widget.pinup = parser_pinup(parser);

	parser_get_gbar_size(parser, &info->gbar.width, &info->gbar.height);

	group = parser_group_str(parser);
	if (group && group_add_widget(group, info->widget_id) < 0) {
		ErrPrint("Failed to build cluster tree for %s{%s}\n", info->widget_id, group);
	}

	parser_unload(parser);
	return WIDGET_ERROR_NONE;
}

HAPI struct pkg_info *package_create(const char *pkgid, const char *widget_id)
{
	struct pkg_info *pkginfo;

	pkginfo = calloc(1, sizeof(*pkginfo));
	if (!pkginfo) {
		ErrPrint("calloc: %d\n", errno);
		return NULL;
	}

	pkginfo->pkgid = strdup(pkgid);
	if (!pkginfo->pkgid) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(pkginfo);
		return NULL;
	}

	pkginfo->widget_id = io_widget_pkgname(widget_id);
	if (!pkginfo->widget_id) {
		ErrPrint("Failed to get pkgname, fallback to fs checker\n");
		pkginfo->widget_id = strdup(widget_id);
		if (!pkginfo->widget_id) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(pkginfo->pkgid);
			DbgFree(pkginfo);
			return NULL;
		}
	}

	package_ref(pkginfo);

	if (io_load_package_db(pkginfo) < 0) {
		ErrPrint("Failed to load DB, fall back to conf file loader\n");
		if (load_conf(pkginfo) < 0) {
			ErrPrint("Failed to initiate the conf file loader\n");
			package_unref(pkginfo);
			return NULL;
		}
	}

	s_info.pkg_list = eina_list_append(s_info.pkg_list, pkginfo);

	return pkginfo;
}

HAPI int package_destroy(struct pkg_info *info)
{
	package_unref(info);
	return WIDGET_ERROR_NONE;
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

HAPI char *package_widget_pkgname(const char *pkgname)
{
	char *widget_id;

	widget_id = io_widget_pkgname(pkgname);
	if (!widget_id) {
		widget_id = strdup(pkgname);
		if (!widget_id) {
			ErrPrint("strdup: %d\n", errno);
			return NULL;
		}
	}

	return widget_id;
}

HAPI int package_is_widget_pkgname(const char *pkgname)
{
	char *widget_id;
	int ret;

	widget_id = package_widget_pkgname(pkgname);
	ret = !!widget_id;
	DbgFree(widget_id);

	return ret;
}

HAPI struct pkg_info *package_find(const char *widget_id)
{
	Eina_List *l;
	struct pkg_info *info;

	if (!widget_id) {
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (!strcmp(info->widget_id, widget_id)) {
			return info;
		}
	}

	return NULL;
}

HAPI struct inst_info *package_find_instance_by_id(const char *widget_id, const char *id)
{
	Eina_List *l;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(widget_id);
	if (!info) {
		ErrPrint("Package %s is not exists\n", widget_id);
		return NULL;
	}

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		if (!strcmp(instance_id(inst), id)) {
			return inst;
		}
	}

	return NULL;
}

HAPI struct inst_info *package_find_instance_by_timestamp(const char *widget_id, double timestamp)
{
	Eina_List *l;
	struct inst_info *inst;
	struct pkg_info *info;

	info = package_find(widget_id);
	if (!info) {
		ErrPrint("Package %s is not exists\n", widget_id);
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
		return WIDGET_ERROR_NOT_EXIST;
	}

	CRITICAL_LOG("=============\n");
	CRITICAL_LOG("faulted at %lf\n", info->fault_info->timestamp);
	CRITICAL_LOG("Package: %s\n", info->widget_id);
	CRITICAL_LOG("Function: %s\n", info->fault_info->function);
	CRITICAL_LOG("InstanceID: %s\n", info->fault_info->filename);
	return WIDGET_ERROR_NONE;
}

static int package_is_faulted(struct pkg_info *info, double timestamp)
{
	if ((timestamp - info->faulted_at) < WIDGET_CONF_FAULT_DETECT_IN_TIME) {
		info->fault_count++;
		DbgPrint("Triggered faulted_in_time (%lf)\n", WIDGET_CONF_FAULT_DETECT_IN_TIME);
	} else {
		info->fault_count = 1;
		info->faulted_at = timestamp;
	}

	if (info->fault_count > WIDGET_CONF_FAULT_DETECT_COUNT) {
		info->fault_count = 0;
		info->faulted_at = timestamp;
		DbgPrint("Triggered fault_count, return 1\n", WIDGET_CONF_FAULT_DETECT_COUNT);
		return 1;
	}

	DbgPrint("Faulted: %d (%d / %lf)\n", info->fault_count, WIDGET_CONF_FAULT_DETECT_COUNT, WIDGET_CONF_FAULT_DETECT_IN_TIME);
	return 0;
}

HAPI int package_get_fault_info(struct pkg_info *info, double *timestamp, const char **filename, const char **function)
{
	if (!info->fault_info) {
		return WIDGET_ERROR_NOT_EXIST;
	}

	*timestamp = info->fault_info->timestamp;
	*filename = info->fault_info->filename;
	*function = info->fault_info->function;
	return WIDGET_ERROR_NONE;
}

HAPI int package_set_fault_info(struct pkg_info *info, double timestamp, const char *filename, const char *function)
{
	struct fault_info *fault;

	if (!package_is_faulted(info, timestamp)) {
		DbgPrint("Faulted but excused\n");
		return WIDGET_ERROR_CANCELED;
	}

	package_clear_fault(info);

	fault = calloc(1, sizeof(*fault));
	if (!fault) {
		ErrPrint("calloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
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
		ErrPrint("strdup: %d\n", errno);
		DbgFree(fault);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	fault->function = strdup(function);
	if (!fault->function) {
		ErrPrint("strdup: %d\n", errno);
		DbgFree(fault->filename);
		DbgFree(fault);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	info->fault_info = fault;
	return WIDGET_ERROR_NONE;
}

HAPI int package_clear_fault(struct pkg_info *info)
{
	if (!info->fault_info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	package_dump_fault_info(info);

	DbgFree(info->fault_info->function);
	DbgFree(info->fault_info->filename);
	DbgFree(info->fault_info);
	info->fault_info = NULL;
	return WIDGET_ERROR_NONE;
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
	return info->widget.timeout;
}

HAPI void package_set_timeout(struct pkg_info *info, int timeout)
{
	info->widget.timeout = timeout;
}

HAPI const double const package_period(const struct pkg_info *info)
{
	return info->widget.period;
}

HAPI void package_set_period(struct pkg_info *info, double period)
{
	info->widget.period = period;
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
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->script);
	info->script = tmp;
	return WIDGET_ERROR_NONE;
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
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->abi);
	info->abi = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI const char * const package_widget_path(const struct pkg_info *info)
{
	if (info->widget.type != WIDGET_TYPE_SCRIPT) {
		return NULL;
	}

	return info->widget.info.script.path;
}

HAPI int package_set_widget_path(struct pkg_info *info, const char *path)
{
	char *tmp;

	if (info->widget.type != WIDGET_TYPE_SCRIPT) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	tmp = strdup(path);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->widget.info.script.path);
	info->widget.info.script.path = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI const char * const package_widget_group(const struct pkg_info *info)
{
	if (info->widget.type != WIDGET_TYPE_SCRIPT) {
		return NULL;
	}

	return info->widget.info.script.group;
}

HAPI int package_set_widget_group(struct pkg_info *info, const char *group)
{
	char *tmp;

	if (info->widget.type != WIDGET_TYPE_SCRIPT) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	tmp = strdup(group);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->widget.info.script.group);
	info->widget.info.script.group = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI const char * const package_gbar_path(const struct pkg_info *info)
{
	if (info->gbar.type != GBAR_TYPE_SCRIPT) {
		return NULL;
	}

	return info->gbar.info.script.path;
}

HAPI int package_set_gbar_path(struct pkg_info *info, const char *path)
{
	char *tmp;

	if (info->gbar.type != GBAR_TYPE_SCRIPT) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	tmp = strdup(path);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->gbar.info.script.path);
	info->gbar.info.script.path = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI const char * const package_gbar_group(const struct pkg_info *info)
{
	if (info->gbar.type != GBAR_TYPE_SCRIPT) {
		return NULL;
	}

	return info->gbar.info.script.group;
}

HAPI int package_set_gbar_group(struct pkg_info *info, const char *group)
{
	char *tmp;

	if (info->gbar.type != GBAR_TYPE_SCRIPT) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	tmp = strdup(group);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->gbar.info.script.group);
	info->gbar.info.script.group = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI const int const package_pinup(const struct pkg_info *info)
{
	return info->widget.pinup;
}

HAPI void package_set_pinup(struct pkg_info *info, int pinup)
{
	info->widget.pinup = pinup;
}

HAPI const char * const package_auto_launch(const struct pkg_info *info)
{
	return info->widget.auto_launch;
}

HAPI void package_set_auto_launch(struct pkg_info *info, const char *auto_launch)
{
	if (!auto_launch) {
		auto_launch = "";
	}

	info->widget.auto_launch = strdup(auto_launch);
	if (!info->widget.auto_launch) {
		ErrPrint("strdup: %d\n", errno);
		return;
	}
}

HAPI const unsigned int const package_size_list(const struct pkg_info *info)
{
	return info->widget.size_list;
}

HAPI void package_set_size_list(struct pkg_info *info, unsigned int size_list)
{
	info->widget.size_list = size_list;
}

HAPI const int const package_gbar_width(const struct pkg_info *info)
{
	return info->gbar.width;
}

HAPI void package_set_gbar_width(struct pkg_info *info, int width)
{
	info->gbar.width = width;
}

HAPI const int const package_gbar_height(const struct pkg_info *info)
{
	return info->gbar.height;
}

HAPI void package_set_gbar_height(struct pkg_info *info, int height)
{
	info->gbar.height = height;
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

HAPI const enum widget_widget_type package_widget_type(const struct pkg_info *info)
{
	return info ? info->widget.type : WIDGET_TYPE_NONE;
}

HAPI void package_set_widget_type(struct pkg_info *info, enum widget_widget_type type)
{
	info->widget.type = type;
}

HAPI const char * const package_libexec(struct pkg_info *info)
{
	return info->widget.libexec;
}

HAPI int package_set_libexec(struct pkg_info *info, const char *libexec)
{
	char *tmp;

	tmp = strdup(libexec);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->widget.libexec);
	info->widget.libexec = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI int package_network(struct pkg_info *info)
{
	return info->network;
}

HAPI void package_set_network(struct pkg_info *info, int network)
{
	info->network = network;
}

HAPI void package_set_direct_input(struct pkg_info *info, int direct_input)
{
	info->direct_input = direct_input;
}

HAPI int package_direct_input(const struct pkg_info *info)
{
	return info->direct_input;
}

HAPI const enum widget_gbar_type const package_gbar_type(const struct pkg_info *info)
{
	return info ? info->gbar.type : GBAR_TYPE_NONE;
}

HAPI void package_set_gbar_type(struct pkg_info *info, enum widget_gbar_type type)
{
	info->gbar.type = type;
}

HAPI const char *package_hw_acceleration(struct pkg_info *info)
{
	return info->hw_acceleration;
}

HAPI int package_set_hw_acceleration(struct pkg_info *info, const char *hw_acceleration)
{
	char *tmp;

	if (!hw_acceleration || !info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	tmp = strdup(hw_acceleration);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->hw_acceleration);
	info->hw_acceleration = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI int package_set_category(struct pkg_info *info, const char *category)
{
	char *tmp;

	if (!category || !info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	tmp = strdup(category);
	if (!tmp) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->category);
	info->category = tmp;
	return WIDGET_ERROR_NONE;
}

HAPI const char *package_category(const struct pkg_info *info)
{
	return info->category;
}

/*!
 * \note
 * Add the instance to the package info.
 * If a package has no slave, assign a new slave.
 */
static inline int assign_new_slave(const char *slave_pkgname, struct pkg_info *info)
{
	char *s_name;
	const char *category;

	s_name = util_slavename();
	if (!s_name) {
		ErrPrint("Failed to get a new slave name\n");
		return WIDGET_ERROR_FAULT;
	}

	DbgPrint("New slave[%s] is assigned for %s (using %s / abi[%s] / accel[%s]\n", s_name, info->widget_id, slave_pkgname, info->abi, info->hw_acceleration);
	info->slave = slave_create(s_name, info->secured, info->abi, slave_pkgname, info->network, info->hw_acceleration);
	DbgFree(s_name);
	if (!info->slave) {
		/*!
		 * \note
		 * package_destroy will try to remove "info" from the pkg_list.
		 * but we didn't add this to it yet.
		 * If the list method couldn't find an "info" from the list,
		 * it just do nothing so I'll leave this.
		 */
		return WIDGET_ERROR_FAULT;
	}

	slave_set_resource_limit(info->slave, 0u, 0u);

	/**
	 * @note
	 * At the first time, the slave is created, "is_watch" will be initialized.
	 */
	category = package_category(info);
	slave_set_is_watch(info->slave, (category && strcmp(CATEGORY_WATCH_CLOCK, category) == 0));

	/*!
	 * \note
	 * Slave is not activated yet.
	 */
	return WIDGET_ERROR_NONE;
}

HAPI int package_add_instance(struct pkg_info *info, struct inst_info *inst)
{
	if (!info->inst_list) {
		char *slave_pkgname;

		slave_pkgname = slave_package_name(info->abi, info->widget_id);
		if (!slave_pkgname) {
			return WIDGET_ERROR_OUT_OF_MEMORY;
		}

		info->slave = slave_find_available(slave_pkgname, info->abi, info->secured, info->network, info->hw_acceleration);
		if (!info->slave) {
			int ret;

			ret = assign_new_slave(slave_pkgname, info);
			DbgFree(slave_pkgname);
			if (ret < 0) {
				return ret;
			}
		} else {
			DbgFree(slave_pkgname);
			DbgPrint("Slave %s is used for %s\n", slave_name(info->slave), info->widget_id);
		}

		(void)slave_ref(info->slave);
		slave_load_package(info->slave);
		(void)slave_event_callback_add(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
		(void)slave_event_callback_add(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);
		(void)slave_event_callback_add(info->slave, SLAVE_EVENT_FAULT, slave_fault_cb, info);

		if (info->secured || (WIDGET_IS_INHOUSE(package_abi(info)) && WIDGET_CONF_SLAVE_LIMIT_TO_TTL)) {
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
	return WIDGET_ERROR_NONE;
}

HAPI int package_del_instance(struct pkg_info *info, struct inst_info *inst)
{
	info->inst_list = eina_list_remove(info->inst_list, inst);

	if (info->inst_list) {
		return WIDGET_ERROR_NONE;
	}

	if (info->slave) {
		slave_unload_package(info->slave);

		slave_event_callback_del(info->slave, SLAVE_EVENT_FAULT, slave_fault_cb, info);
		slave_event_callback_del(info->slave, SLAVE_EVENT_DEACTIVATE, slave_deactivated_cb, info);
		slave_event_callback_del(info->slave, SLAVE_EVENT_ACTIVATE, slave_activated_cb, info);

		if (info->secured || (WIDGET_IS_INHOUSE(package_abi(info)) && WIDGET_CONF_SLAVE_LIMIT_TO_TTL)) {
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

	return WIDGET_ERROR_NONE;
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
			fault_unicast_info(client, info->widget_id, info->fault_info->filename, info->fault_info->function);
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
					if (client_is_subscribed_group(client, instance_cluster(inst), instance_category(inst))
						|| client_is_subscribed_by_category(client, package_category(instance_package(inst))))
					{
						instance_unicast_created_event(inst, client);
						DbgPrint("(Subscribed) Created package: %s\n", info->widget_id);
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

static int io_uninstall_cb(const char *pkgid, const char *widget_id, int prime, void *data)
{
	struct pkg_info *info;
	Eina_List *l;
	Eina_List *n;
	struct inst_info *inst;

	DbgPrint("Package %s is uninstalled\n", widget_id);
	info = package_find(widget_id);
	if (!info) {
		DbgPrint("%s is not yet loaded\n", widget_id);
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
			instance_destroy(inst, WIDGET_DESTROY_TYPE_UNINSTALL);
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
	widget_size_type_e size_type;
	int width;
	int height;
	double old_period;

	DbgPrint("Already exists, try to update it\n");

	old_period = info->widget.period;

	group_del_widget(info->widget_id);
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
		width = instance_widget_width(inst);
		height = instance_widget_height(inst);
		DbgPrint("%dx%d\n", width, height);
		if (widget_service_get_size_type(width, height, &size_type) == WIDGET_ERROR_NONE) {
			if ((info->widget.size_list & size_type) == size_type) {
				DbgPrint("Supported size type: %x\n", size_type);
				if (instance_period(inst) == old_period) {
					instance_reload_period(inst, package_period(info));
				}
				instance_reload(inst, WIDGET_DESTROY_TYPE_UPGRADE);
			} else {
				DbgPrint("Unsupported size type: %x\n", size_type);
				instance_destroy(inst, WIDGET_DESTROY_TYPE_UNINSTALL);
			}
		} else {
			DbgPrint("Invalid size\n");
			instance_destroy(inst, WIDGET_DESTROY_TYPE_UNINSTALL);
		}
	}
}

static int io_install_cb(const char *pkgid, const char *widget_id, int prime, void *data)
{
	struct pkg_info *info;

	info = package_find(widget_id);
	if (info) {
		/*!
		 * Already exists. skip to create this.
		 */
		return 0;
	}

	info = package_create(pkgid, widget_id);
	if (!info) {
		ErrPrint("Failed to build an info %s\n", widget_id);
	} else {
		DbgPrint("widget %s is built\n", widget_id);
	}

	return 0;
}

static int uninstall_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	if (status == PKGMGR_STATUS_START) {
		Eina_List *l;
		Eina_List *n;
		struct pkg_info *info;

		EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, n, info) {
			if (!strcmp(info->pkgid, pkgname)) {
				struct slave_node *slave;
				/**
				 * @note
				 * While updating packages, the slave can be terminated,
				 * Even if a slave is terminated, do not handles it as a faulted one.
				 */
				slave = package_slave(info);
				DbgPrint("Update slave state : %p\n", slave);
				if (slave) {
					slave_set_wait_deactivation(slave, 1);
				}
			}
		}
	} else if (status == PKGMGR_STATUS_END) {
		Eina_List *l;
		Eina_List *n;
		struct pkg_info *info;

		EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, n, info) {
			if (!strcmp(info->pkgid, pkgname)) {
				struct slave_node *slave;
				slave = package_slave(info);
				if (slave) {
					slave_set_wait_deactivation(slave, 0);
				}
				io_uninstall_cb(pkgname, info->widget_id, -1, NULL);
			}
		}
	}

	return 0;
}

static int update_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	if (status == PKGMGR_STATUS_START) {
		Eina_List *l;
		Eina_List *n;
		struct pkg_info *info;

		EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, n, info) {
			if (!strcmp(info->pkgid, pkgname)) {
				struct slave_node *slave;
				/**
				 * @note
				 * While updating packages, the slave can be terminated,
				 * Even if a slave is terminated, do not handles it as a faulted one.
				 */
				slave = package_slave(info);
				DbgPrint("Update slave state : %p\n", slave);
				if (slave) {
					slave_set_wait_deactivation(slave, 1);
				}
			}
		}
	} else if (status == PKGMGR_STATUS_END) {
		Eina_List *l;
		Eina_List *n;
		struct pkg_info *info;

		EINA_LIST_FOREACH_SAFE(s_info.pkg_list, l, n, info) {
			if (!strcmp(info->pkgid, pkgname)) {
				struct slave_node *slave;

				slave = package_slave(info);
				if (slave) {
					slave_set_wait_deactivation(slave, 0);
				}

				DbgPrint("Update widget_id: %s\n", info->widget_id);
				if (io_is_exists(info->widget_id) == 1) {
					reload_package_info(info);
				} else {
					io_uninstall_cb(pkgname, info->widget_id, -1, NULL);
				}
			}
		}

		(void)io_update_widget_package(pkgname, io_install_cb, NULL);
	}

	return 0;
}

static int crawling_widgetes(const char *pkgid, const char *widget_id, int prime, void *data)
{
	if (package_find(widget_id)) {
		ErrPrint("Information of %s is already built\n", widget_id);
	} else {
		struct pkg_info *info;
		info = package_create(pkgid, widget_id);
		if (info) {
			DbgPrint("[%s] information is built prime(%d)\n", widget_id, prime);
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

	io_crawling_widgetes(crawling_widgetes, NULL);
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
			instance_destroy(inst, WIDGET_DESTROY_TYPE_TERMINATE);
		}

		package_destroy(info);
	}

	return 0;
}

/**
 * Find a standalone provider
 */
HAPI const char *package_find_by_secured_slave(struct slave_node *slave)
{
	Eina_List *l;
	struct pkg_info *info;

	if (!slave_is_secured(slave) && !slave_is_app(slave)) {
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.pkg_list, l, info) {
		if (info->slave == slave) {
			return info->widget_id;
		}
	}

	return NULL;
}

HAPI const char * const package_name(const struct pkg_info *info)
{
	return info->widget_id;
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
			if (instance_client(inst) == client) {
				DbgPrint("Skip owner\n");
				continue;
			}

			if (!client_is_subscribed_group(client, instance_cluster(inst), instance_category(inst))
				&& !client_is_subscribed_by_category(client, package_category(instance_package(inst))))
			{
				DbgPrint("Not subscribed [%s]\n", package_category(instance_package(inst)));
				continue;
			}

			switch (instance_state(inst)) {
			case INST_INIT:
			case INST_REQUEST_TO_ACTIVATE:
				/* Will be send a created event after the instance gets created event */
				switch (alter) {
				case ALTER_CREATE:
					DbgPrint("ALTER_CREATE\n");
					if (!instance_has_client(inst, client)) {
						instance_add_client(inst, client);
					}
					break;
				case ALTER_DESTROY:
					DbgPrint("ALTER_DESTROY\n");
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
					DbgPrint("ALTER_CREATE\n");
					if (!instance_has_client(inst, client)) {
						instance_unicast_created_event(inst, client);
						instance_add_client(inst, client);
						DbgPrint("(Subscribed) Created package: %s\n", info->widget_id);
					}
					break;
				case ALTER_DESTROY:
					DbgPrint("ALTER_DESTROY\n");
					if (instance_has_client(inst, client)) {
						instance_unicast_deleted_event(inst, client, WIDGET_ERROR_NONE);
						instance_del_client(inst, client);
					}
					break;
				default:
					break;
				}

				break;
			default:
				DbgPrint("%s(%s) is not activated (%d)\n", package_name(info), instance_id(inst), instance_state(inst));
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

HAPI int package_instance_count(struct pkg_info *info)
{
	Eina_List *l;
	struct inst_info *inst;
	int count = 0;

	EINA_LIST_FOREACH(info->inst_list, l, inst) {
		switch (instance_state(inst)) {
		case INST_INIT:
		case INST_ACTIVATED:
		case INST_REQUEST_TO_ACTIVATE:
		case INST_REQUEST_TO_REACTIVATE:
			count++;
			break;
		case INST_DESTROYED:
		case INST_REQUEST_TO_DESTROY:
		default:
			break;
		}
	}

	return count;
}

HAPI int package_is_enabled(const char *appid)
{
	pkgmgrinfo_appinfo_h handle;
	bool enabled;
	int ret;

	ret = pkgmgrinfo_appinfo_get_appinfo(appid, &handle);
	if (ret != PMINFO_R_OK) {
		ErrPrint("Failed to get info\n");
		return 0;
	}

	ret = pkgmgrinfo_appinfo_is_enabled(handle, &enabled);
	if (ret != PMINFO_R_OK) {
		ErrPrint("Failed to get info\n");
		enabled = false;
	}

	pkgmgrinfo_appinfo_destroy_appinfo(handle);
	return enabled == true;
}

HAPI char *package_meta_tag(const char *appid, const char *meta_tag)
{
	char *ret = NULL;
	char *value = NULL;
	int status;
	pkgmgrinfo_appinfo_h handle;

	if (!meta_tag || !appid) {
		ErrPrint("meta(%s) is not valid (%s)\n", meta_tag, appid);
		return NULL;
	}

	status = pkgmgrinfo_appinfo_get_appinfo(appid, &handle);
	if (status != PMINFO_R_OK) {
		return NULL;
	}

	status = pkgmgrinfo_appinfo_get_metadata_value(handle, meta_tag, &value);
	if (status != PMINFO_R_OK) {
		pkgmgrinfo_appinfo_destroy_appinfo(handle);
		return NULL;
	}

	if (value && value[0] != '\0') {
		ret = strdup(value);
	}

	pkgmgrinfo_appinfo_destroy_appinfo(handle);
	return ret;
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
		return WIDGET_ERROR_FAULT;
	}

	/* Emulated fault routine */
	// (void)package_set_fault_info(pkg, util_timestamp(), slave_name(slave), __func__);
	if (broadcast) {
		fault_broadcast_info(package_name(pkg), slave_name(slave), __func__);
	}

	DbgPrint("package: %s (forucely faulted %s)\n", package_name(pkg), slave_name(slave));
	EINA_LIST_FOREACH_SAFE(pkg->inst_list, l, n, inst) {
		DbgPrint("Destroy instance %p\n", inst);
		instance_destroy(inst, WIDGET_DESTROY_TYPE_FAULT);
	}

	return WIDGET_ERROR_NONE;
}

HAPI char *package_get_pkgid(const char *appid)
{
	int ret;
	pkgmgrinfo_appinfo_h handle;
	char *new_appid = NULL;
	char *pkgid = NULL;

	ret = pkgmgrinfo_appinfo_get_appinfo(appid, &handle);
	if (ret != PMINFO_R_OK) {
		ErrPrint("Failed to get appinfo\n");
		return NULL;
	}

	ret = pkgmgrinfo_appinfo_get_pkgid(handle, &new_appid);
	if (ret != PMINFO_R_OK) {
		pkgmgrinfo_appinfo_destroy_appinfo(handle);
		ErrPrint("Failed to get pkgname for (%s)\n", appid);
		return NULL;
	}

	if (new_appid && new_appid[0] != '\0') {
		pkgid = strdup(new_appid);
		if (!pkgid) {
			ErrPrint("strdup: %d\n", errno);
		}
	}
	pkgmgrinfo_appinfo_destroy_appinfo(handle);

	return pkgid;
}

/* End of a file */
