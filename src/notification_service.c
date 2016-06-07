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

static GHashTable *_monitoring_hash = NULL;
static int _update_noti(GVariant **reply_body, notification_h noti, GList *monitoring_list);

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
	DbgPrint("name vanished : %s, %d", name);
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
	int uid = get_sender_uid(sender);
	GList *monitoring_list = NULL;

	monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, &uid);
	if (g_strcmp0(method_name, "noti_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender,
				_on_name_appeared, _on_name_vanished, &_monitoring_hash, uid);
	else if (g_strcmp0(method_name, "update_noti") == 0)
		ret = notification_update_noti(parameters, &reply_body, monitoring_list, uid);
	else if (g_strcmp0(method_name, "add_noti") == 0)
		ret = notification_add_noti(parameters, &reply_body, monitoring_list, uid);
	else if (g_strcmp0(method_name, "refresh_noti") == 0)
		ret = notification_refresh_noti(parameters, &reply_body, monitoring_list, uid);
	else if (g_strcmp0(method_name, "del_noti_single") == 0)
		ret = notification_del_noti_single(parameters, &reply_body, monitoring_list, uid);
	else if (g_strcmp0(method_name, "del_noti_multiple") == 0)
		ret = notification_del_noti_multiple(parameters, &reply_body, monitoring_list, uid);
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

static int _validate_uid(notification_h noti, uid_t uid)
{
	int ret;
	uid_t noti_uid = 0;

	ret = notification_get_uid(noti, &noti_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("notification_get_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	if (uid > NORMAL_UID_BASE && uid != noti_uid) {
		ErrPrint("_validate_uid invalid seder uid : %d, noti_uid : %d", uid, noti_uid);
		return NOTIFICATION_ERROR_IO_ERROR;
	}
	return ret;
}

static int _validate_uid_with_param_uid(uid_t uid, uid_t param_uid)
{
	int ret = NOTIFICATION_ERROR_NONE;
	if (uid > NORMAL_UID_BASE && uid != param_uid) {
		ErrPrint("_validate_uid_with_param_uid invalid seder uid : %d, noti_uid : %d", uid, param_uid);
		return NOTIFICATION_ERROR_IO_ERROR;
	}
	return ret;
}

int notification_add_noti(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid)
{
	int ret;
	notification_h noti;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		g_variant_unref(body);

		ret = _validate_uid(noti, uid);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("_validate_uid fail ret : %d", ret);
			notification_free(noti);
			return NOTIFICATION_ERROR_IO_ERROR;
		}

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

int notification_update_noti(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid)
{
	notification_h noti;
	int ret;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		g_variant_unref(body);

		ret = _validate_uid(noti, uid);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("_validate_uid fail ret : %d", ret);
			notification_free(noti);
			return NOTIFICATION_ERROR_IO_ERROR;
		}

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
		ret = _validate_uid_with_param_uid(uid, param_uid);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("_validate_uid fail ret : %d", ret);
			return NOTIFICATION_ERROR_IO_ERROR;
		}

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
		ret = _validate_uid_with_param_uid(uid, param_uid);
		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("_validate_uid fail ret : %d", ret);
			return NOTIFICATION_ERROR_IO_ERROR;
		}

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
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}
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
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

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
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}
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
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

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
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}
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
int notification_refresh_noti(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid)
{
	int ret;
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
int notification_del_noti_single(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid)
{
	int ret;
	int num_changes = 0;
	int priv_id = NOTIFICATION_PRIV_ID_NONE;
	char *pkgname = NULL;
	GVariant *body = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&sii)", &pkgname, &priv_id, &param_uid);
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	ret = notification_noti_delete_by_priv_id_get_changes(pkgname, priv_id, &num_changes, param_uid);
	DbgPrint("priv_id: [%d] num_delete:%d\n", priv_id, num_changes);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
		return ret;
	}

	if (num_changes > 0) {
		body = g_variant_new("(ii)", 1, priv_id);
		if (body == NULL) {
			ErrPrint("cannot make gvariant to noti");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
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
int notification_del_noti_multiple(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid)
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

	g_variant_get(parameters, "(&sii)", &pkgname, &type, &param_uid);
	DbgPrint("pkgname: [%s] type: [%d]\n", pkgname, type);
	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

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
		deleted_noti_list = g_variant_new("(a(i))", builder);
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

	g_variant_get(parameters, "(i&sii)", &type, &pkgname, &group_id, &priv_id, &param_uid);
	ret = _validate_uid_with_param_uid(uid, param_uid);
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

	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

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

/* update_noti_sys_setting */
int notification_update_noti_sys_setting(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret;
	int do_not_disturb = 0;
	int visivility_class = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(iii)",
			&do_not_disturb,
			&visivility_class,
			&param_uid);

	ret = _validate_uid_with_param_uid(uid, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("_validate_uid fail ret : %d", ret);
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	DbgPrint("do_not_disturb [%d] visivility_class [%d]\n", do_not_disturb, visivility_class);
	ret = notification_setting_db_update_system_setting(do_not_disturb, visivility_class, param_uid);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db update system setting : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_update_noti_sys_setting_service done !! %d", ret);
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

static int _package_install_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	notification_setting_insert_package(pkgname);
	return 0;
}

static int _package_uninstall_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	notification_setting_delete_package(pkgname);
	return 0;
}

/*!
 * MAIN THREAD
 * Do not try to do any other operation in these functions
 */
HAPI int notification_service_init(void)
{
	int result;

	_monitoring_hash = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, free_monitoring_list);
	result = notification_db_init();
	if (result != NOTIFICATION_ERROR_NONE) {
		ErrPrint("notification db init fail %d", result);
		return result;
	}

	result = notification_register_dbus_interface();
	if (result != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("notification register dbus fail %d", result);
		return NOTIFICATION_ERROR_IO_ERROR;
	}
	_notification_data_init();
	notification_setting_refresh_setting_table();

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
