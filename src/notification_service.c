/*
 * Copyright 2016  Samsung Electronics Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dlog.h>
#include <sys/smack.h>

#include <pkgmgr-info.h>

#include <notification.h>
#include <gio/gio.h>
#include <alarm.h>
#include <aul.h>

#include "pkgmgr.h"
#include "service_common.h"
#include "notification_service.h"
#include "debug.h"

#include <notification_noti.h>
#include <notification_internal.h>
#include <notification_ipc.h>
#include <notification_setting_service.h>
#include <notification_db.h>

#define PROVIDER_NOTI_INTERFACE_NAME "org.tizen.data_provider_noti_service"
#define NOTI_TEMPLATE_LIMIT 10
#define BUF_LEN 256

static GHashTable *_monitoring_hash = NULL;
static int _update_noti(GVariant **reply_body, notification_h noti, GList *monitoring_list);

static alarm_id_t dnd_schedule_start_alarm_id;
static alarm_id_t dnd_schedule_end_alarm_id;

/*!
 * SERVICE HANDLER
 */

/*!
 * NOTIFICATION SERVICE INITIALIZATION
 */

static void _on_name_appeared(GDBusConnection *connection,
		const gchar     *name,
		const gchar     *name_owner,
		gpointer         user_data)
{
	DbgPrint("name appeared : %s", name);
}

static void _on_name_vanished(GDBusConnection *connection,
		const gchar     *name,
		gpointer         user_data)
{
	DbgPrint("name vanished : %s", name);
	monitoring_info_s *info = (monitoring_info_s *)user_data;
	if (info) {
		DbgPrint("name vanished uid : %d", info->uid);
		g_bus_unwatch_name(info->watcher_id);
	}
}

static void _noti_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	/* TODO : sender authority(privilege) check */
	DbgPrint("notification method_name: %s, sender : %s ", method_name, sender);

	GVariant *reply_body = NULL;
	int ret = NOTIFICATION_ERROR_INVALID_OPERATION;
	uid_t uid = get_sender_uid(sender);
	pid_t pid = get_sender_pid(sender);

	if (g_strcmp0(method_name, "noti_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender,
				_on_name_appeared, _on_name_vanished, &_monitoring_hash, uid);
	else if (g_strcmp0(method_name, "update_noti") == 0)
		ret = notification_update_noti(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "add_noti") == 0)
		ret = notification_add_noti(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "refresh_noti") == 0)
		ret = notification_refresh_noti(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "del_noti_single") == 0)
		ret = notification_del_noti_single(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "del_noti_multiple") == 0)
		ret = notification_del_noti_multiple(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_noti_count") == 0)
		ret = notification_get_noti_count(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "update_noti_setting") == 0)
		ret = notification_update_noti_setting(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "update_noti_sys_setting") == 0)
		ret = notification_update_noti_sys_setting(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "load_noti_by_tag") == 0)
		ret = notification_load_noti_by_tag(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "load_noti_by_priv_id") == 0)
		ret = notification_load_noti_by_priv_id(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "load_noti_grouping_list") == 0)
		ret = notification_load_grouping_list(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "load_noti_detail_list") == 0)
		ret = notification_load_detail_list(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_setting_array") == 0)
		ret = notification_get_setting_array(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_setting_by_package_name") == 0)
		ret = notification_get_setting_by_package_name(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "load_system_setting") == 0)
		ret = notification_load_system_setting(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "save_as_template") == 0)
		ret = notification_add_noti_template(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "create_from_template") == 0)
		ret = notification_get_noti_template(parameters, &reply_body, pid, uid);
	else if (g_strcmp0(method_name, "create_from_package_template") == 0)
		ret = notification_get_noti_package_template(parameters, &reply_body, uid);

	if (ret == NOTIFICATION_ERROR_NONE) {
		DbgPrint("notification service success : %d", ret);
		g_dbus_method_invocation_return_value(
				invocation, reply_body);
	} else {
		DbgPrint("notification service fail : %d", ret);
		g_dbus_method_invocation_return_error(
				invocation,
				NOTIFICATION_ERROR,
				ret,
				"notification service error");
	}

}

static const GDBusInterfaceVTable _noti_interface_vtable = {
		_noti_dbus_method_call_handler,
		NULL,
		NULL
};

int notification_register_dbus_interface()
{
	static gchar introspection_xml[] =
			"  <node>"
			"  <interface name='org.tizen.data_provider_noti_service'>"
			"        <method name='noti_service_register'>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='add_noti'>"
			"          <arg type='v' name='noti' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='update_noti'>"
			"          <arg type='v' name='noti' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='refresh_noti'>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='del_noti_single'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='del_noti_multiple'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_by_tag'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='tag' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_by_priv_id'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_grouping_list'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='a(v)' name='noti_list' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_detail_list'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='a(v)' name='noti_list' direction='out'/>"
			"        </method>"

			"        <method name='get_noti_count'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='ret_count' direction='out'/>"
			"        </method>"

			"        <method name='update_noti_setting'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='allow_to_notify' direction='in'/>"
			"          <arg type='i' name='do_not_disturb_except' direction='in'/>"
			"          <arg type='i' name='visibility_class' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='update_noti_sys_setting'>"
			"          <arg type='i' name='do_not_disturb' direction='in'/>"
			"          <arg type='i' name='visibility_class' direction='in'/>"
			"          <arg type='i' name='dnd_schedule_enabled' direction='in'/>"
			"          <arg type='i' name='dnd_schedule_day' direction='in'/>"
			"          <arg type='i' name='dnd_start_hour' direction='in'/>"
			"          <arg type='i' name='dnd_start_min' direction='in'/>"
			"          <arg type='i' name='dnd_end_hour' direction='in'/>"
			"          <arg type='i' name='dnd_end_min' direction='in'/>"
			"          <arg type='i' name='lock_screen_level' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='get_setting_array'>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='setting_cnt' direction='out'/>"
			"          <arg type='a(v)' name='setting_arr' direction='out'/>"
			"        </method>"

			"        <method name='get_setting_by_package_name'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='v' name='setting' direction='out'/>"
			"        </method>"

			"        <method name='load_system_setting'>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='v' name='setting' direction='out'/>"
			"        </method>"

			"        <method name='save_as_template'>"
			"          <arg type='v' name='noti' direction='in'/>"
			"          <arg type='s' name='name' direction='in'/>"
			"        </method>"

			"        <method name='create_from_template'>"
			"          <arg type='s' name='name' direction='in'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"

			"        <method name='create_from_package_template'>"
			"          <arg type='s' name='appid' direction='in'/>"
			"          <arg type='s' name='name' direction='in'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"

			"        <method name='post_toast'>"
			"        </method>"
			"  </interface>"
			"  </node>";

	return service_common_register_dbus_interface(introspection_xml, _noti_interface_vtable);
}

/* add noti */
static int _add_noti(GVariant **reply_body, notification_h noti, GList *monitoring_list)
{
	int ret;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;
	GVariant *body = NULL;

	print_noti(noti);
	ret = notification_noti_insert(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to update a notification:%d\n", ret);
		return ret;
	}

	body = notification_ipc_make_gvariant_from_noti(noti, true);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "add_noti_notify", monitoring_list, PROVIDER_NOTI_INTERFACE_NAME);
	g_variant_unref(body);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", priv_id);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_insert_noti done !!");
	return ret;
}

static int _validate_and_set_noti_with_uid(uid_t uid, notification_h noti, uid_t *noti_uid)
{
	int ret = NOTIFICATION_ERROR_NONE;

	ret = notification_get_uid(noti, noti_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("notification_get_uid fail ret : %d", ret);
		return ret;
	}

	if (uid > NORMAL_UID_BASE && uid != *noti_uid) {
		ErrPrint("invalid seder uid : %d, noti_uid : %d", uid, *noti_uid);
		return NOTIFICATION_ERROR_INVALID_PARAMETER;
	} else if (uid <= NORMAL_UID_BASE) {
		if (*noti_uid <= NORMAL_UID_BASE) {
			if (*noti_uid != uid)
				return NOTIFICATION_ERROR_INVALID_PARAMETER;

			DbgPrint("system defualt noti post change noti_uid to default");
			/*
			IF system (uid <= NORMAL_UID_BASE) try to send noti without specipic destination uid (using notification_pot API)
			Send it to default user (TZ_SYS_DEFAULT_USER)
			*/
			*noti_uid = tzplatform_getuid(TZ_SYS_DEFAULT_USER);
			ret = notification_set_uid(noti, *noti_uid);
			if (ret != NOTIFICATION_ERROR_NONE) {
				ErrPrint("notification_set_uid fail ret : %d", ret);
				return ret;
			}
			DbgPrint("changed noti_uid %d", *noti_uid);
		}
	}
	return ret;
}

static int _validate_and_set_param_uid_with_uid(uid_t uid, uid_t *param_uid)
{
	int ret = NOTIFICATION_ERROR_NONE;
	if (uid > NORMAL_UID_BASE && uid != *param_uid) {
		ErrPrint("invalid seder uid : %d, param_uid : %d", uid, *param_uid);
		return NOTIFICATION_ERROR_INVALID_PARAMETER;
	} else if (uid <= NORMAL_UID_BASE) {
		if (*param_uid <= NORMAL_UID_BASE) {
			if (*param_uid != uid)
				return NOTIFICATION_ERROR_INVALID_PARAMETER;
			/*
			IF system (uid <= NORMAL_UID_BASE) try to send noti without specipic destination uid (using notification_pot API)
			Send it to default user (TZ_SYS_DEFAULT_USER)
			*/
			*param_uid = tzplatform_getuid(TZ_SYS_DEFAULT_USER);
		}
	}
	return ret;
}

int notification_add_noti(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	notification_h noti;
	GVariant *body = NULL;
	GList *monitoring_list = NULL;
	uid_t noti_uid = 0;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		g_variant_unref(body);

		ret = _validate_and_set_noti_with_uid(uid, noti, &noti_uid);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, GUINT_TO_POINTER(noti_uid));
		if (ret == NOTIFICATION_ERROR_NONE) {
			ret = notification_noti_check_tag(noti);
			if (ret == NOTIFICATION_ERROR_NOT_EXIST_ID)
				ret = _add_noti(reply_body, noti, monitoring_list);
			else if (ret == NOTIFICATION_ERROR_ALREADY_EXIST_ID)
				ret = _update_noti(reply_body, noti, monitoring_list);
		}
		notification_free(noti);

	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("notification_add_noti ret : %d", ret);
	return ret;
}

/* update noti */
static int _update_noti(GVariant **reply_body, notification_h noti, GList *monitoring_list)
{
	int ret;
	GVariant *body = NULL;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;

	print_noti(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	ret = notification_noti_update(noti);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	body = notification_ipc_make_gvariant_from_noti(noti, true);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	ret = send_notify(body, "update_noti_notify", monitoring_list, PROVIDER_NOTI_INTERFACE_NAME);
	g_variant_unref(body);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", priv_id);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_update_noti done !!");
	return ret;
}

int notification_update_noti(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	notification_h noti;
	int ret;
	GVariant *body = NULL;
	uid_t noti_uid;
	GList *monitoring_list = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		g_variant_unref(body);

		ret = _validate_and_set_noti_with_uid(uid, noti, &noti_uid);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, GUINT_TO_POINTER(noti_uid));
		if (ret == NOTIFICATION_ERROR_NONE)
			ret = _update_noti(reply_body, noti, monitoring_list);

		notification_free(noti);
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* load_noti_by_tag */
int notification_load_noti_by_tag(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	char *tag = NULL;
	char *pkgname = NULL;
	notification_h noti;
	uid_t param_uid;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(&s&si)", &pkgname, &tag, &param_uid);
		ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		DbgPrint("_load_noti_by_tag pkgname : %s, tag : %s ", pkgname, tag);
		ret = notification_noti_get_by_tag(noti, pkgname, tag, param_uid);

		DbgPrint("notification_noti_get_by_tag ret : %d", ret);
		print_noti(noti);

		*reply_body = notification_ipc_make_gvariant_from_noti(noti, true);
		notification_free(noti);

		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_load_noti_by_tag done !!");
	return ret;
}

/* load_noti_by_priv_id */
int notification_load_noti_by_priv_id(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;
	char *pkgname = NULL;
	notification_h noti;
	uid_t param_uid;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(&sii)", &pkgname, &priv_id, &param_uid);
		ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		DbgPrint("load_noti_by_priv_id pkgname : %s, priv_id : %d ", pkgname, priv_id);
		ret = notification_noti_get_by_priv_id(noti, pkgname, priv_id, param_uid);

		DbgPrint("notification_noti_get_by_priv_id ret : %d", ret);
		print_noti(noti);

		*reply_body = notification_ipc_make_gvariant_from_noti(noti, true);
		notification_free(noti);

		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_load_noti_by_priv_id done !!");
	return ret;
}

/* load_noti_grouping_list */
int notification_load_grouping_list(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	notification_h noti;
	GVariant *body = NULL;
	notification_type_e type = NOTIFICATION_TYPE_NONE;
	notification_list_h get_list = NULL;
	notification_list_h list_iter;
	GVariantBuilder *builder;
	int count = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(iii)", &type, &count, &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;
	DbgPrint("load grouping list type : %d, count : %d ", type, count);

	ret = notification_noti_get_grouping_list(type, count, &get_list, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));
	if (get_list) {
		list_iter = notification_list_get_head(get_list);
		do {
			noti = notification_list_get_data(list_iter);
			body = notification_ipc_make_gvariant_from_noti(noti, true);
			g_variant_builder_add(builder, "(v)", body);

			list_iter = notification_list_get_next(list_iter);
		} while (list_iter != NULL);

		notification_free_list(get_list);
	}

	*reply_body = g_variant_new("(a(v))", builder);
	g_variant_builder_unref(builder);

	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("load grouping list done !!");
	return ret;
}

/* get_setting_array */
int notification_get_setting_array(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	GVariant *body;
	GVariantBuilder *builder;
	int count = 0;
	int i;
	notification_setting_h setting_array = NULL;
	notification_setting_h temp;
	uid_t param_uid;

	g_variant_get(parameters, "(i)", &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	ret = noti_setting_get_setting_array(&setting_array, &count, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	DbgPrint("get setting array : %d", count);
	builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));

	if (setting_array) {
		for (i = 0; i < count; i++) {
			temp = setting_array + i;
			body = notification_ipc_make_gvariant_from_setting(temp);
			g_variant_builder_add(builder, "(v)", body);

			if (temp->package_name)
				free(temp->package_name);
		}
		free(setting_array);
	}
	*reply_body = g_variant_new("(ia(v))", count, builder);
	g_variant_builder_unref(builder);

	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_setting_array */
int notification_get_setting_by_package_name(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	GVariant *body;
	char *pkgname = NULL;
	notification_setting_h setting = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&si)", &pkgname, &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;
	DbgPrint("get setting by pkgname : %s", pkgname);

	ret = noti_setting_service_get_setting_by_package_name(pkgname, &setting, param_uid);
	if (ret == NOTIFICATION_ERROR_NONE) {
		body = notification_ipc_make_gvariant_from_setting(setting);
		notification_setting_free_notification(setting);

		if (body == NULL) {
			ErrPrint("fail to make gvariant");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
	} else {
		return ret;
	}

	*reply_body = g_variant_new("(v)", body);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* load_system_setting */
int notification_load_system_setting(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	GVariant *body;
	notification_system_setting_h setting = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(i)", &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	ret = noti_system_setting_load_system_setting(&setting, param_uid);
	if (ret == NOTIFICATION_ERROR_NONE) {
		body = notification_ipc_make_gvariant_from_system_setting(setting);
		notification_system_setting_free_system_setting(setting);

		if (body == NULL) {
			ErrPrint("fail to make gvariant");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
	} else {
		return ret;
	}

	*reply_body = g_variant_new("(v)", body);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("load system setting done !!");
	return ret;
}

/* load_noti_detail_list */
int notification_load_detail_list(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	notification_h noti;
	GVariant *body = NULL;
	notification_list_h get_list = NULL;
	notification_list_h list_iter;
	GVariantBuilder *builder;
	char *pkgname = NULL;
	int group_id = NOTIFICATION_GROUP_ID_NONE;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;
	int count = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(&siiii)", &pkgname, &group_id, &priv_id, &count, &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;
	DbgPrint("load detail list pkgname : %s, group_id : %d, priv_id : %d, count : %d ",
			pkgname, group_id, priv_id, count);

	ret = notification_noti_get_detail_list(pkgname, group_id, priv_id,
			count, &get_list, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));

	if (get_list) {
		list_iter = notification_list_get_head(get_list);
		do {
			noti = notification_list_get_data(list_iter);
			body = notification_ipc_make_gvariant_from_noti(noti, true);
			if (body) {
				g_variant_builder_add(builder, "(v)", body);
				list_iter = notification_list_get_next(list_iter);
			}
		} while (list_iter != NULL);
		notification_free_list(get_list);
	}

	*reply_body = g_variant_new("(a(v))", builder);
	g_variant_builder_unref(builder);

	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("load detail list done !!");
	return ret;
}

/* refresh_noti */
int notification_refresh_noti(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	uid_t param_uid;
	GList *monitoring_list = NULL;

	g_variant_get(parameters, "(i)", &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, GUINT_TO_POINTER(param_uid));
	ret = send_notify(parameters, "refresh_noti_notify", monitoring_list, PROVIDER_NOTI_INTERFACE_NAME);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_refresh_noti_service done !!");
	return ret;
}

/* del_noti_single */
int notification_del_noti_single(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	int num_changes = 0;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;
	char *pkgname = NULL;
	GVariant *body = NULL;
	uid_t param_uid;
	GList *monitoring_list = NULL;

	g_variant_get(parameters, "(&sii)", &pkgname, &priv_id, &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;
	ret = notification_noti_delete_by_priv_id_get_changes(pkgname, priv_id, &num_changes, param_uid);
	DbgPrint("priv_id: [%d] num_delete:%d\n", priv_id, num_changes);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
		return ret;
	}

	if (num_changes > 0) {
		body = g_variant_new("(iii)", 1, priv_id, param_uid);
		if (body == NULL) {
			ErrPrint("cannot make gvariant to noti");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
		monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, GUINT_TO_POINTER(param_uid));
		ret = send_notify(body, "delete_single_notify", monitoring_list, PROVIDER_NOTI_INTERFACE_NAME);
		g_variant_unref(body);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to send notify:%d\n", ret);
			return ret;
		}
	}

	*reply_body = g_variant_new("(i)", priv_id);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_del_noti_single done !!");
	return ret;
}

/* del_noti_multiple */
int notification_del_noti_multiple(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	char *pkgname = NULL;
	notification_type_e type = NOTIFICATION_TYPE_NONE;
	int num_deleted = 0;
	int *list_deleted = NULL;
	GVariant *deleted_noti_list;
	GVariantBuilder *builder;
	int i;
	uid_t param_uid;
	GList *monitoring_list = NULL;

	g_variant_get(parameters, "(&sii)", &pkgname, &type, &param_uid);
	DbgPrint("pkgname: [%s] type: [%d]\n", pkgname, type);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	ret = notification_noti_delete_all(type, pkgname, &num_deleted, &list_deleted, param_uid);
	DbgPrint("ret: [%d] num_deleted: [%d]\n", ret, num_deleted);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete notifications:%d\n", ret);
		if (list_deleted != NULL)
			free(list_deleted);
		return ret;
	}

	if (num_deleted > 0) {
		builder = g_variant_builder_new(G_VARIANT_TYPE("a(i)"));

		for (i = 0; i < num_deleted; i++) {
			g_variant_builder_add(builder, "(i)", *(list_deleted + i));
		}
		deleted_noti_list = g_variant_new("(a(i)i)", builder, param_uid);
		monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, GUINT_TO_POINTER(param_uid));
		ret = send_notify(deleted_noti_list, "delete_multiple_notify", monitoring_list, PROVIDER_NOTI_INTERFACE_NAME);

		g_variant_builder_unref(builder);
		g_variant_unref(deleted_noti_list);
		free(list_deleted);

		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to send notify:%d\n", ret);
			return ret;
		}
	}

	*reply_body = g_variant_new("(i)", num_deleted);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_del_noti_multiple done !!");
	return ret;
}

/* get_noti_count */
int notification_get_noti_count(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	notification_type_e type = NOTIFICATION_TYPE_NONE;
	char *pkgname = NULL;
	int group_id = NOTIFICATION_GROUP_ID_NONE;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;
	int noti_count = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(i&siii)", &type, &pkgname, &group_id, &priv_id, &param_uid);
	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	ret = notification_noti_get_count(type, pkgname, group_id, priv_id,
			&noti_count, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to get count : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", noti_count);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_get_noti_property_service done !! %d", ret);
	return ret;
}

/* update_noti_setting */
int notification_update_noti_setting(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	char *pkgname = NULL;
	int allow_to_notify = 0;
	int do_not_disturb_except = 0;
	int visivility_class = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(&siiii)",
			&pkgname,
			&allow_to_notify,
			&do_not_disturb_except,
			&visivility_class,
			&param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	DbgPrint("package_name: [%s] allow_to_notify: [%d] do_not_disturb_except: [%d] visivility_class: [%d]\n",
			pkgname, allow_to_notify, do_not_disturb_except, visivility_class);
	ret = notification_setting_db_update(pkgname, allow_to_notify, do_not_disturb_except, visivility_class, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db update : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_update_noti_setting_service done !! %d", ret);
	return ret;
}

static int _dnd_schedule_alarm_cb(alarm_id_t alarm_id, void *data)
{
	/* need to get current uid, use default user here. temporarily */
	if (alarm_id == dnd_schedule_start_alarm_id) {
		notification_setting_db_update_do_not_disturb(1, tzplatform_getuid(TZ_SYS_DEFAULT_USER));
	} else if (alarm_id == dnd_schedule_end_alarm_id) {
		notification_setting_db_update_do_not_disturb(0, tzplatform_getuid(TZ_SYS_DEFAULT_USER));
	} else {
		ErrPrint("notification wrong alarm [%d]", alarm_id);
		return -1;
	}

	return 0;
}

static int _get_current_time(struct tm *date)
{
	time_t now;

	if (date == NULL) {
		ErrPrint("NOTIFICATION_ERROR_INVALID_PARAMETER");
		return NOTIFICATION_ERROR_INVALID_PARAMETER;
	}

	time(&now);
	localtime_r(&now, date);

	return NOTIFICATION_ERROR_NONE;
}

static int _noti_system_setting_set_alarm(int week_flag, int hour, int min, alarm_cb_t handler, alarm_id_t *dnd_schedule_alarm_id)
{
	int err = NOTIFICATION_ERROR_NONE;
	struct tm struct_time;
	alarm_entry_t *alarm_info = NULL;
	alarm_date_t alarm_time;
	alarm_id_t alarm_id = -1;

	err = alarmmgr_init("notification");
	if (err < 0) {
		ErrPrint("alarmmgr_init failed [%d]", err);
		goto out;
	}

	err = alarmmgr_set_cb(handler, NULL);
	if (err < 0) {
		ErrPrint("alarmmgr_set_cb failed [%d]", err);
		goto out;
	}

	err = _get_current_time(&struct_time);
	if (err != NOTIFICATION_ERROR_NONE) {
		ErrPrint("get_current_time failed");
		goto out;
	}

	alarm_info = alarmmgr_create_alarm();
	if (alarm_info == NULL) {
		ErrPrint("alarmmgr_create_alarm failed");
		goto out;
	}

	alarm_time.year = struct_time.tm_year + 1900;
	alarm_time.month = struct_time.tm_mon + 1;
	alarm_time.day = struct_time.tm_mday;
	alarm_time.hour = hour;
	alarm_time.min = min;
	alarm_time.sec = 0;

	err = alarmmgr_set_time(alarm_info, alarm_time);
	if (err != ALARMMGR_RESULT_SUCCESS) {
		ErrPrint("alarmmgr_set_time failed (%d)", err);
		goto out;
	}

	if (week_flag) {
		err = alarmmgr_set_repeat_mode(alarm_info, ALARM_REPEAT_MODE_WEEKLY, week_flag);
		if (err != ALARMMGR_RESULT_SUCCESS) {
			ErrPrint("alarmmgr_set_repeat_mode failed [%d]", err);
			goto out;
		}
	}

	err = alarmmgr_set_type(alarm_info, ALARM_TYPE_VOLATILE);
	if (err != ALARMMGR_RESULT_SUCCESS) {
		ErrPrint("alarmmgr_set_type failed [%d]", err);
		goto out;
	}

	err = alarmmgr_add_alarm_with_localtime(alarm_info, NULL, &alarm_id);
	if (err != ALARMMGR_RESULT_SUCCESS) {
		ErrPrint("alarmmgr_add_alarm_with_localtime failed [%d]", err);
		goto out;
	}

	*dnd_schedule_alarm_id = alarm_id;

	DbgPrint("alarm_id [%d]", *dnd_schedule_alarm_id);

out:
	if (alarm_info)
		alarmmgr_free_alarm(alarm_info);

	return err;
}

static int _add_alarm(int dnd_schedule_day, int dnd_start_hour, int dnd_start_min, int dnd_end_hour, int dnd_end_min)
{
	int ret = NOTIFICATION_ERROR_NONE;

	if (dnd_schedule_start_alarm_id)
		alarmmgr_remove_alarm(dnd_schedule_start_alarm_id);

	ret = _noti_system_setting_set_alarm(dnd_schedule_day,
				dnd_start_hour, dnd_start_min,
				_dnd_schedule_alarm_cb, &dnd_schedule_start_alarm_id);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_add_alarm fail %d", ret);
		return ret;
	}

	if (dnd_schedule_end_alarm_id)
		alarmmgr_remove_alarm(dnd_schedule_end_alarm_id);

	ret = _noti_system_setting_set_alarm(dnd_schedule_day,
				dnd_end_hour, dnd_end_min,
				_dnd_schedule_alarm_cb, &dnd_schedule_end_alarm_id);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_add_alarm fail %d", ret);
		return ret;
	}

	return ret;
}

/* update_noti_sys_setting */
int notification_update_noti_sys_setting(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	int do_not_disturb = 0;
	int visivility_class = 0;
	int dnd_schedule_enabled = 0;
	int dnd_schedule_day = 0;
	int dnd_start_hour = 0;
	int dnd_start_min = 0;
	int dnd_end_hour = 0;
	int dnd_end_min = 0;
	int lock_screen_level = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(iiiiiiiiii)",
				&do_not_disturb,
				&visivility_class,
				&dnd_schedule_enabled,
				&dnd_schedule_day,
				&dnd_start_hour,
				&dnd_start_min,
				&dnd_end_hour,
				&dnd_end_min,
				&lock_screen_level,
				&param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	DbgPrint("do_not_disturb [%d] visivility_class [%d] set_schedule [%d] lock_screen_level [%d]\n",
			do_not_disturb, visivility_class, dnd_schedule_enabled, lock_screen_level);

	ret = notification_setting_db_update_system_setting(do_not_disturb,
				visivility_class,
				dnd_schedule_enabled,
				dnd_schedule_day,
				dnd_start_hour,
				dnd_start_min,
				dnd_end_hour,
				dnd_end_min,
				lock_screen_level,
				param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db update system setting : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	if (dnd_schedule_enabled) {
		ret = _add_alarm(dnd_schedule_day, dnd_start_hour, dnd_start_min,
				dnd_end_hour, dnd_end_min);
		if (ret != NOTIFICATION_ERROR_NONE)
			ErrPrint("failed to add alarm for dnd_schedule");
	}

	DbgPrint("_update_noti_sys_setting_service done !! %d", ret);

	return ret;
}

int notification_add_noti_template(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	notification_h noti;
	int ret;
	GVariant *body = NULL;
	GVariant *coupled_body = NULL;
	char *template_name = NULL;
	int count = 0;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v&s)", &coupled_body, &template_name);
		g_variant_get(coupled_body, "(v)", &body);

		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		g_variant_unref(coupled_body);
		g_variant_unref(body);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to make a notification:%d\n", ret);
			return ret;
		}

		ret = notification_noti_check_count_for_template(noti, &count);
		if (count > NOTI_TEMPLATE_LIMIT)
			return NOTIFICATION_ERROR_MAX_EXCEEDED;

		ret = notification_noti_add_template(noti, template_name);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to add a notification:%d\n", ret);
			return ret;
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

int notification_get_noti_template(GVariant *parameters, GVariant **reply_body, pid_t pid, uid_t uid)
{
	int ret;
	char appid[BUF_LEN] = {0, };
	char *template_name = NULL;
	notification_h noti;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(&s)", &template_name);

		ret = aul_app_get_appid_bypid_for_uid(pid, appid, sizeof(appid), uid);
		if (ret != AUL_R_OK) {
			ErrPrint("failed to get appid:%d", ret);
			return ret;
		}

		ret = notification_noti_get_package_template(noti, appid, template_name);
		if (ret != NOTIFICATION_ERROR_NONE) {
			DbgPrint("failed to get template:%d", ret);
			return ret;
		}

		*reply_body = notification_ipc_make_gvariant_from_noti(noti, false);
		notification_free(noti);

		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_get_noti_template done !!");
	return ret;
}

int notification_get_noti_package_template(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	char *pkgname = NULL;
	char *template_name = NULL;
	notification_h noti;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(&s&s)", &pkgname, &template_name);

		ret = notification_noti_get_package_template(noti, pkgname, template_name);
		if (ret != NOTIFICATION_ERROR_NONE) {
			DbgPrint("failed to get template:%d", ret);
			return ret;
		}

		*reply_body = notification_ipc_make_gvariant_from_noti(noti, false);
		notification_free(noti);

		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_get_noti_package_template done !!");
	return ret;
}

static void _notification_data_init(void)
{
	int property = 0;
	int priv_id = 0;
	char *noti_pkgname = NULL;
	notification_h noti = NULL;
	notification_list_h noti_list = NULL;
	notification_list_h noti_list_head = NULL;
	notification_type_e noti_type = NOTIFICATION_TYPE_NONE;

	notification_noti_get_grouping_list(NOTIFICATION_TYPE_NONE, -1, &noti_list, NOTIFICATION_GLOBAL_UID);
	noti_list_head = noti_list;

	while (noti_list != NULL) {
		noti = notification_list_get_data(noti_list);
		if (noti) {
			notification_get_id(noti, NULL, &priv_id);
			notification_get_pkgname(noti, &noti_pkgname);
			notification_get_property(noti, &property);
			notification_get_type(noti, &noti_type);

			if (noti_type == NOTIFICATION_TYPE_ONGOING
					|| property & NOTIFICATION_PROP_VOLATILE_DISPLAY) {
				notification_noti_delete_by_priv_id(noti_pkgname, priv_id);
			}
		}
		noti_list = notification_list_get_next(noti_list);
	}

	if (noti_list_head != NULL)
		notification_free_list(noti_list_head);
}

static int _package_install_cb(uid_t uid, const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	notification_setting_insert_package_for_uid(pkgname, uid);
	return 0;
}

static int _package_uninstall_cb(uid_t uid, const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	notification_setting_delete_package_for_uid(pkgname, uid);
	notification_noti_delete_template(pkgname);
	return 0;
}

static int _check_dnd_schedule(void)
{
	int ret;
	notification_system_setting_h setting = NULL;
	bool dnd_schedule_enabled = false;
	int dnd_schedule_day = 0;
	int dnd_start_hour = 0;
	int dnd_start_min = 0;
	int dnd_end_hour = 0;
	int dnd_end_min = 0;

	ret = noti_system_setting_load_system_setting(&setting,
				tzplatform_getuid(TZ_SYS_DEFAULT_USER));
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("noti_system_setting_load_system_setting fail %d", ret);
		return ret;
	}

	ret = notification_system_setting_dnd_schedule_get_enabled(setting,
				&dnd_schedule_enabled);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("system_setting_dnd_schedule_get_enabled fail %d", ret);
		goto out;
	}

	if (dnd_schedule_enabled) {
		ret = notification_system_setting_dnd_schedule_get_day(setting,
				&dnd_schedule_day);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("system_setting_dnd_schedule_get_day fail %d", ret);
			goto out;
		}

		ret = notification_system_setting_dnd_schedule_get_start_time(setting,
				&dnd_start_hour, &dnd_start_min);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("system_setting_dnd_schedule_get_start_time fail %d", ret);
			goto out;
		}

		ret = notification_system_setting_dnd_schedule_get_end_time(setting,
				&dnd_end_hour, &dnd_end_min);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("system_setting_dnd_schedule_get_end_time fail %d", ret);
			goto out;
		}

		_add_alarm(dnd_schedule_day, dnd_start_hour, dnd_start_min, dnd_end_hour, dnd_end_min);
	}

out:
	notification_system_setting_free_system_setting(setting);

	return ret;
}

/*!
 * MAIN THREAD
 * Do not try to do any other operation in these functions
 */
HAPI int notification_service_init(void)
{
	int ret;

	_monitoring_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free_monitoring_list);
	ret = notification_db_init();
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("notification db init fail %d", ret);
		return ret;
	}

	ret = notification_register_dbus_interface();
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("notification register dbus fail %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	_notification_data_init();
	notification_setting_refresh_setting_table(tzplatform_getuid(TZ_SYS_DEFAULT_USER));

	_check_dnd_schedule();

	pkgmgr_init();
	pkgmgr_add_event_callback(PKGMGR_EVENT_INSTALL, _package_install_cb, NULL);
	pkgmgr_add_event_callback(PKGMGR_EVENT_UPDATE, _package_install_cb, NULL);
	pkgmgr_add_event_callback(PKGMGR_EVENT_UNINSTALL, _package_uninstall_cb, NULL);
	DbgPrint("Successfully initiated\n");
	return NOTIFICATION_ERROR_NONE;
}

HAPI int notification_service_fini(void)
{
	pkgmgr_fini();
	DbgPrint("Successfully Finalized\n");
	return NOTIFICATION_ERROR_NONE;
}

/* End of a file */
