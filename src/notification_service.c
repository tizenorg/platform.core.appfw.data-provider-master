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

#define NOTI_IPC_OBJECT_PATH "/org/tizen/noti_service"

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"

static int _update_noti(GVariant **reply_body, notification_h noti);

/*!
 * SERVICE HANDLER
 */

static void _on_bus_acquired(GDBusConnection *connection,
		const gchar *name, gpointer user_data)
{
	DbgPrint("_on_bus_acquired : %s", name);
}

static void _on_name_acquired(GDBusConnection *connection,
		const gchar *name, gpointer user_data)
{
	DbgPrint("_on_name_acquired : %s", name);
}

static void _on_name_lost(GDBusConnection *connection,
		const gchar *name, gpointer user_data)
{
	DbgPrint("_on_name_lost : %s", name);
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
	int ret = NOTIFICATION_ERROR_NONE;

	if (g_strcmp0(method_name, "noti_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender, NOTIFICATION_SERVICE);
	else if (g_strcmp0(method_name, "update_noti") == 0)
		ret = notification_update_noti(parameters, &reply_body);
	else if (g_strcmp0(method_name, "add_noti") == 0)
		ret = notification_add_noti(parameters, &reply_body);
	else if (g_strcmp0(method_name, "refresh_noti") == 0)
		ret = notification_refresh_noti(parameters, &reply_body);
	else if (g_strcmp0(method_name, "del_noti_single") == 0)
		ret = notification_del_noti_single(parameters, &reply_body);
	else if (g_strcmp0(method_name, "del_noti_multiple") == 0)
		ret = notification_del_noti_multiple(parameters, &reply_body);
	else if (g_strcmp0(method_name, "set_noti_property") == 0)
		ret = notification_set_noti_property(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_noti_property") == 0)
		ret = notification_get_noti_property(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_noti_count") == 0)
		ret = notification_get_noti_count(parameters, &reply_body);
	else if (g_strcmp0(method_name, "update_noti_setting") == 0)
		ret = notification_update_noti_setting(parameters, &reply_body);
	else if (g_strcmp0(method_name, "update_noti_sys_setting") == 0)
		ret = notification_update_noti_sys_setting(parameters, &reply_body);
	else if (g_strcmp0(method_name, "load_noti_by_tag") == 0)
		ret = notification_load_noti_by_tag(parameters, &reply_body);
	else if (g_strcmp0(method_name, "load_noti_by_priv_id") == 0)
		ret = notification_load_noti_by_priv_id(parameters, &reply_body);
	else if (g_strcmp0(method_name, "load_noti_grouping_list") == 0)
		ret = notification_load_grouping_list(parameters, &reply_body);
	else if (g_strcmp0(method_name, "load_noti_detail_list") == 0)
		ret = notification_load_detail_list(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_setting_array") == 0)
		ret = notification_get_setting_array(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_setting_by_package_name") == 0)
		ret = notification_get_setting_by_package_name(parameters, &reply_body);
	else if (g_strcmp0(method_name, "load_system_setting") == 0)
		ret = notification_load_system_setting(parameters, &reply_body);

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

int notification_register_dbus_interface(GDBusConnection *connection)
{
	int result = SERVICE_COMMON_ERROR_NONE;
	GDBusNodeInfo *introspection_data = NULL;
	int noti_registration_id = 0;

	static gchar introspection_xml[] =
			"  <node>"
			"  <interface name='org.tizen.data_provider_noti_service'>"
			"        <method name='noti_service_register'>"
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
			"        </method>"

			"        <method name='del_noti_single'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='del_noti_multiple'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_by_tag'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='tag' direction='in'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_by_priv_id'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_grouping_list'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"          <arg type='a(v)' name='noti_list' direction='out'/>"
			"        </method>"

			"        <method name='load_noti_detail_list'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"          <arg type='a(v)' name='noti_list' direction='out'/>"
			"        </method>"

			"        <method name='set_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='in'/>"
			"        </method>"

			"        <method name='get_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='ret_value' direction='out'/>"
			"        </method>"

			"        <method name='get_noti_count'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='ret_count' direction='out'/>"
			"        </method>"

			"        <method name='update_noti_setting'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='allow_to_notify' direction='in'/>"
			"          <arg type='i' name='do_not_disturb_except' direction='in'/>"
			"          <arg type='i' name='visibility_class' direction='in'/>"
			"        </method>"

			"        <method name='update_noti_sys_setting'>"
			"          <arg type='i' name='do_not_disturb' direction='in'/>"
			"          <arg type='i' name='visibility_class' direction='in'/>"
			"        </method>"

			"        <method name='get_setting_array'>"
			"          <arg type='i' name='setting_cnt' direction='out'/>"
			"          <arg type='a(v)' name='setting_arr' direction='out'/>"
			"        </method>"

			"        <method name='get_setting_by_package_name'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='v' name='setting' direction='out'/>"
			"        </method>"

			"        <method name='load_system_setting'>"
			"          <arg type='v' name='setting' direction='out'/>"
			"        </method>"

			"        <method name='post_toast'>"
			"        </method>"
			"  </interface>"
			"  </node>";

	int owner_id = 0;
	GError *error = NULL;

	owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
			PROVIDER_BUS_NAME,
			G_BUS_NAME_OWNER_FLAGS_NONE,
			_on_bus_acquired,
			_on_name_acquired,
			_on_name_lost,
			NULL, NULL);
	if (!owner_id) {
		ErrPrint("g_bus_own_name error");
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		goto out;
	}

	DbgPrint("Acquiring the own name : %d", owner_id);
	introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
	if (!introspection_data) {
		ErrPrint("g_dbus_node_info_new_for_xml() is failed.");
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		if (error != NULL) {
			ErrPrint("g_dbus_node_info_new_for_xml error [%s]", error->message);
			g_error_free(error);
		}
		goto out;
	}

	noti_registration_id = g_dbus_connection_register_object(connection,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[0],
			&_noti_interface_vtable, NULL, NULL, NULL);

	DbgPrint("noti_registration_id %d", noti_registration_id);
	if (noti_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		goto out;
	}

out:
	if (introspection_data)
		g_dbus_node_info_unref(introspection_data);

	return result;
}


/* add noti */
static int _add_noti(GVariant **reply_body, notification_h noti)
{
	int ret = 0;
	int priv_id = 0;
	GVariant *body = NULL;

	print_noti(noti);
	ret = notification_noti_insert(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to update a notification:%d\n", ret);
		return ret;
	}

	body = notification_ipc_make_gvariant_from_noti(noti);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "add_noti_notify", NOTIFICATION_SERVICE);
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

int notification_add_noti(GVariant *parameters, GVariant **reply_body)
{
	int ret = 0;
	notification_h noti = NULL;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		if (ret == NOTIFICATION_ERROR_NONE) {
			ret = notification_noti_check_tag(noti);
			if (ret == NOTIFICATION_ERROR_NOT_EXIST_ID)
				ret = _add_noti(reply_body, noti);
			else if (ret == NOTIFICATION_ERROR_ALREADY_EXIST_ID)
				ret = _update_noti(reply_body, noti);

			notification_free(noti);
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}


	DbgPrint("notification_add_noti ret : %d", ret);
	return ret;
}


/* update noti */
static int _update_noti(GVariant **reply_body, notification_h noti)
{
	int ret = 0;
	GVariant *body = NULL;
	int priv_id = 0;

	print_noti(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	ret = notification_noti_update(noti);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	body = notification_ipc_make_gvariant_from_noti(noti);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	ret = send_notify(body, "update_noti_notify", NOTIFICATION_SERVICE);
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

int notification_update_noti(GVariant *parameters, GVariant **reply_body)
{
	notification_h noti = NULL;
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		if (ret == NOTIFICATION_ERROR_NONE) {
				ret = _update_noti(reply_body, noti);
				notification_free(noti);
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* load_noti_by_tag */
int notification_load_noti_by_tag(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *tag;
	char *pkgname;
	notification_h noti = NULL;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(ss)", &pkgname, &tag);
		DbgPrint("_load_noti_by_tag pkgname : %s, tag : %s ", pkgname, tag);
		ret = notification_noti_get_by_tag(noti, pkgname, tag);

		DbgPrint("notification_noti_get_by_tag ret : %d", ret);
		print_noti(noti);

		body = notification_ipc_make_gvariant_from_noti(noti);
		*reply_body = g_variant_new("(v)", body);
		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}

		notification_free(noti);
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_load_noti_by_tag done !!");
	return ret;
}

/* load_noti_by_priv_id */
int notification_load_noti_by_priv_id(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	int priv_id;
	char *pkgname;
	notification_h noti = NULL;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(si)", &pkgname, &priv_id);
		DbgPrint("load_noti_by_priv_id pkgname : %s, priv_id : %d ", pkgname, priv_id);
		ret = notification_noti_get_by_priv_id(noti, pkgname, priv_id);

		DbgPrint("notification_noti_get_by_priv_id ret : %d", ret);
		print_noti(noti);

		body = notification_ipc_make_gvariant_from_noti(noti);
		*reply_body = g_variant_new("(v)", body);
		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}

		notification_free(noti);
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_load_noti_by_priv_id done !!");
	return ret;
}

/* load_noti_grouping_list */
int notification_load_grouping_list(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	notification_h noti = NULL;
	GVariant *body = NULL;
	notification_type_e type;
	notification_list_h get_list = NULL;
	notification_list_h list_iter = NULL;
	GVariantBuilder *builder = NULL;
	int count;
	int noti_cnt = 0;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(ii)", &type, &count);
		DbgPrint("load grouping list type : %d, count : %d ", type, count);

		ret = notification_noti_get_grouping_list(type, count, &get_list);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));
		if (get_list) {

			list_iter = notification_list_get_head(get_list);
			do {
				noti = notification_list_get_data(list_iter);
				body = notification_ipc_make_gvariant_from_noti(noti);
				g_variant_builder_add(builder, "(v)", body);
				list_iter = notification_list_get_next(list_iter);
				noti_cnt++;
			} while (list_iter != NULL);

		}
		*reply_body = g_variant_new("(a(v))", builder);
		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
		notification_free_list(get_list);

	} else {
		ErrPrint("cannot create notification");
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("load grouping list done !!");
	return ret;
}

/* get_setting_array */
int notification_get_setting_array(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;
	GVariantBuilder *builder = NULL;
	int count;
	int i = 0;
	notification_setting_h setting_array = NULL;
	notification_setting_h temp = NULL;

	ret = noti_setting_get_setting_array(&setting_array, &count);
	if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

	DbgPrint("get setting array : %d", count);
	builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));

	if (setting_array) {
		for (i = 0; i < count; i++) {
			temp = setting_array + i;
			body = notification_ipc_make_gvariant_from_setting(temp);
			g_variant_builder_add(builder, "(v)", body);
		}
	}
	*reply_body = g_variant_new("(ia(v))", count, builder);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_setting_array */
int notification_get_setting_by_package_name(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;
	char *pkgname = NULL;
	notification_setting_h setting = NULL;

	g_variant_get(parameters, "(s)", &pkgname);
	DbgPrint("get setting by pkgname : %s", pkgname);

	ret = noti_setting_service_get_setting_by_package_name(pkgname, &setting);
	if (ret == NOTIFICATION_ERROR_NONE) {
		body = notification_ipc_make_gvariant_from_setting(setting);
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
int notification_load_system_setting(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;
	notification_system_setting_h setting = NULL;

	ret = noti_system_setting_load_system_setting(&setting);
	if (ret == NOTIFICATION_ERROR_NONE) {
		body = notification_ipc_make_gvariant_from_system_setting(setting);
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

/* load_noti_detail_list */
int notification_load_detail_list(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	notification_h noti = NULL;
	GVariant *body = NULL;
	notification_list_h get_list = NULL;
	notification_list_h list_iter = NULL;
	GVariantBuilder *builder = NULL;
	char *pkgname = NULL;
	int group_id;
	int priv_id;
	int count;
	int noti_cnt = 0;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(siii)", &pkgname, &group_id, &priv_id, &count);
		DbgPrint("load detail list pkgname : %s, group_id : %d, priv_id : %d, count : %d ",
				pkgname, group_id, priv_id, count);

		ret = notification_noti_get_detail_list(pkgname, group_id, priv_id,
				count, &get_list);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));
		if (get_list) {

			list_iter = notification_list_get_head(get_list);
			do {
				noti = notification_list_get_data(list_iter);
				body = notification_ipc_make_gvariant_from_noti(noti);
				g_variant_builder_add(builder, "(v)", body);
				list_iter = notification_list_get_next(list_iter);
				noti_cnt++;
			} while (list_iter != NULL);

		}
		*reply_body = g_variant_new("(a(v))", builder);
		if (*reply_body == NULL) {
			ErrPrint("cannot make reply_body");
			return NOTIFICATION_ERROR_OUT_OF_MEMORY;
		}
		notification_free_list(get_list);

	} else {
		ErrPrint("cannot create notification");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("load detail list done !!");
	return ret;
}

/* refresh_noti */
int notification_refresh_noti(GVariant *parameters, GVariant **reply_body)
{
	int ret = 0;
	ret = send_notify(parameters, "refresh_noti_notify", NOTIFICATION_SERVICE);
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
int notification_del_noti_single(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	int num_changes = 0;
	int priv_id = 0;
	char *pkgname = NULL;
	GVariant *body = NULL;

	g_variant_get(parameters, "(si)", &pkgname, &priv_id);
	pkgname = string_get(pkgname);
	ret = notification_noti_delete_by_priv_id_get_changes(pkgname, priv_id, &num_changes);
	DbgPrint("priv_id: [%d] num_delete:%d\n", priv_id, num_changes);
	if (pkgname)
		g_free(pkgname);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
		return ret;
	}

	if  (num_changes <= 0) {
		ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
		return NOTIFICATION_ERROR_NOT_EXIST_ID;
	}

	body = g_variant_new("(ii)", 1, priv_id);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "delete_single_notify", NOTIFICATION_SERVICE);
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
	DbgPrint("_del_noti_single done !!");
	return ret;
}

/* del_noti_multiple */
int notification_del_noti_multiple(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *pkgname = NULL;
	notification_type_e type = 0;
	int num_deleted = 0;
	int *list_deleted = NULL;
	GVariant *deleted_noti_list;
	int i = 0;

	g_variant_get(parameters, "(si)", &pkgname, &type);
	pkgname = string_get(pkgname);

	DbgPrint("pkgname: [%s] type: [%d]\n", pkgname, type);

	ret = notification_noti_delete_all(type, pkgname, &num_deleted, &list_deleted);
	DbgPrint("ret: [%d] num_deleted: [%d]\n", ret, num_deleted);
	if (pkgname)
		g_free(pkgname);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete notifications:%d\n", ret);
		if (list_deleted != NULL)
			free(list_deleted);
		return ret;
	}

	if (num_deleted > 0) {
		GVariantBuilder * builder = g_variant_builder_new(G_VARIANT_TYPE("a(i)"));
		for (i = 0; i < num_deleted; i++) {
			g_variant_builder_add(builder, "(i)", *(list_deleted + i));
		}
		deleted_noti_list = g_variant_new("(a(i))", builder);
		ret = send_notify(deleted_noti_list, "delete_multiple_notify", NOTIFICATION_SERVICE);

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

/* set_noti_property */
int notification_set_noti_property(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	g_variant_get(parameters, "(sss)", &pkgname, &property, &value);
	pkgname = string_get(pkgname);
	property = string_get(property);
	value = string_get(value);

	ret = notification_setting_db_set(pkgname, property, value);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db set : %d\n", ret);
		return ret;
	}
	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_set_noti_property_service done !! %d", ret);
	return ret;
}

/* get_noti_property */
int notification_get_noti_property(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &property);
	pkgname = string_get(pkgname);
	property = string_get(property);

	ret = notification_setting_db_get(pkgname, property, &value);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db get : %d\n", ret);
		return ret;
	}
	*reply_body = g_variant_new("(s)", value);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	if (value != NULL)
		free(value);

	DbgPrint("_get_noti_property_service done !! %d", ret);
	return ret;
}

/* get_noti_count */
int notification_get_noti_count(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	notification_type_e type;
	char *pkgname;
	int group_id;
	int priv_id;
	int noti_count;

	g_variant_get(parameters, "(isii)", &type, &pkgname, &group_id, &priv_id);
	pkgname = string_get(pkgname);

	ret = notification_noti_get_count(type, pkgname, group_id, priv_id,
			&noti_count);
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
int notification_update_noti_setting(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *pkgname = NULL;
	int allow_to_notify = 0;
	int do_not_disturb_except = 0;
	int visivility_class = 0;

	g_variant_get(parameters, "(siii)",
			&pkgname,
			&allow_to_notify,
			&do_not_disturb_except,
			&visivility_class);

	pkgname = string_get(pkgname);
	DbgPrint("package_name: [%s] allow_to_notify: [%d] do_not_disturb_except: [%d] visivility_class: [%d]\n",
			pkgname, allow_to_notify, do_not_disturb_except, visivility_class);
	ret = notification_setting_db_update(pkgname, allow_to_notify, do_not_disturb_except, visivility_class);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db update : %d\n", ret);
		return ret;
	}

	if (pkgname)
		g_free(pkgname);

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_update_noti_setting_service done !! %d", ret);
	return ret;
}

/* update_noti_sys_setting */
int notification_update_noti_sys_setting(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	int do_not_disturb = 0;
	int visivility_class = 0;

	g_variant_get(parameters, "(ii)",
			&do_not_disturb,
			&visivility_class);

	DbgPrint("do_not_disturb [%d] visivility_class [%d]\n", do_not_disturb, visivility_class);
	ret = notification_setting_db_update_system_setting(do_not_disturb, visivility_class);
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

/*!
 * NOTIFICATION SERVICE INITIALIZATION
 */
static void _notification_data_init(void)
{
	int property = 0;
	int priv_id = 0;
	char *noti_pkgname = NULL;
	notification_h noti = NULL;
	notification_list_h noti_list = NULL;
	notification_list_h noti_list_head = NULL;
	notification_type_e noti_type = NOTIFICATION_TYPE_NONE;

	notification_noti_get_grouping_list(NOTIFICATION_TYPE_NONE, -1, &noti_list);
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
	/*struct info *notification_service_info = (struct info *)data;

	if (status != PKGMGR_STATUS_END)
		return 0;

	_invoke_package_change_event(notification_service_info, PKGMGR_EVENT_INSTALL, pkgname);*/

	return 0;
}

static int _package_uninstall_cb(const char *pkgname, enum pkgmgr_status status, double value, void *data)
{
	notification_setting_delete_package(pkgname);
	/*struct info *notification_service_info = (struct info *)data;

	if (status != PKGMGR_STATUS_END)
		return 0;

	_invoke_package_change_event(notification_service_info, PKGMGR_EVENT_UNINSTALL, pkgname);*/

	return 0;
}

/*!
 * MAIN THREAD
 * Do not try to do any other operation in these functions
 */
HAPI int notification_service_init(void)
{
	/*if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return SERVICE_COMMON_ERROR_ALREADY_STARTED;
	}*/

	_notification_data_init();
	notification_setting_refresh_setting_table();

	pkgmgr_init();
	pkgmgr_add_event_callback(PKGMGR_EVENT_INSTALL, _package_install_cb, NULL);
	pkgmgr_add_event_callback(PKGMGR_EVENT_UPDATE, _package_install_cb, NULL);
	pkgmgr_add_event_callback(PKGMGR_EVENT_UNINSTALL, _package_uninstall_cb, NULL);
	DbgPrint("Successfully initiated\n");
	return SERVICE_COMMON_ERROR_NONE;
}

HAPI int notification_service_fini(void)
{
	pkgmgr_fini();
	DbgPrint("Successfully Finalized\n");
	return SERVICE_COMMON_ERROR_NONE;
}

/* End of a file */
