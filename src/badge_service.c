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

#include <dlog.h>
#include <gio/gio.h>
#include <sys/smack.h>
#include <badge.h>
#include <badge_db.h>
#include <badge_setting_service.h>
#include <badge_internal.h>

#include "service_common.h"
#include "badge_service.h"
#include "debug.h"

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"
#define PROVIDER_BADGE_INTERFACE_NAME "org.tizen.data_provider_badge_service"

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

static void _badge_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	/* TODO : sender authority(privilege) check */
	DbgPrint("badge method_name: %s", method_name);

	GVariant *reply_body = NULL;
	int ret = BADGE_ERROR_NONE;

	if (g_strcmp0(method_name, "badge_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender, BADGE_SERVICE);
	else if (g_strcmp0(method_name, "insert_badge") == 0)
		ret = badge_insert(parameters, &reply_body);
	else if (g_strcmp0(method_name, "delete_badge") == 0)
		ret = badge_delete(parameters, &reply_body);
	else if (g_strcmp0(method_name, "set_badge_count") == 0)
		ret = badge_set_badge_count(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_badge_count") == 0)
		ret = badge_get_badge_count(parameters, &reply_body);
	else if (g_strcmp0(method_name, "set_disp_option") == 0)
		ret = badge_set_display_option(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_disp_option") == 0)
		ret = badge_get_display_option(parameters, &reply_body);
	else if (g_strcmp0(method_name, "set_noti_property") == 0)
		ret = badge_set_setting_property(parameters, &reply_body);
	else if (g_strcmp0(method_name, "get_noti_property") == 0)
		ret = badge_get_setting_property(parameters, &reply_body);

	if (ret == BADGE_ERROR_NONE) {
		DbgPrint("badge service success : %d", ret);
		g_dbus_method_invocation_return_value(
				invocation, reply_body);
	} else {
		DbgPrint("badge service fail : %d", ret);
		g_dbus_method_invocation_return_error(
				invocation,
				BADGE_ERROR,
				ret,
				"badge service error");
	}
}

static const GDBusInterfaceVTable _badge_interface_vtable = {
		_badge_dbus_method_call_handler,
		NULL,
		NULL
};

int badge_register_dbus_interface(GDBusConnection *connection)
{
	int result = SERVICE_COMMON_ERROR_NONE;
	GDBusNodeInfo *introspection_data = NULL;
	int badge_registration_id = 0;

	static gchar introspection_xml[] =
			"  <node>"
			"  <interface name='org.tizen.data_provider_badge_service'>"
			"        <method name='badge_service_register'>"
			"        </method>"

			"        <method name='insert_badge'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='writable_pkg' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"        </method>"

			"        <method name='delete_badge'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"        </method>"

			"        <method name='set_badge_count'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"        </method>"

			"        <method name='get_badge_count'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='count' direction='out'/>"
			"        </method>"

			"        <method name='set_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='is_display' direction='in'/>"
			"        </method>"

			"        <method name='get_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='is_display' direction='out'/>"
			"        </method>"

			"        <method name='set_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='in'/>"
			"        </method>"

			"        <method name='get_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='out'/>"
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

	badge_registration_id = g_dbus_connection_register_object(connection,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[0],
			&_badge_interface_vtable, NULL, NULL, NULL);
	DbgPrint("badge_registration_id %d", badge_registration_id);
	if (badge_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		goto out;
	}

out:
	if (introspection_data)
		g_dbus_node_info_unref(introspection_data);

	return result;
}

/* insert_badge */
int badge_insert(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *writable_pkg = NULL;
	char *caller = NULL;
	GVariant *body = NULL;

	g_variant_get(parameters, "(sss)", &pkgname, &writable_pkg, &caller);
	pkgname = string_get(pkgname);
	writable_pkg = string_get(writable_pkg);
	caller = string_get(caller);

	if (pkgname != NULL && writable_pkg != NULL && caller != NULL)
		ret = badge_db_insert(pkgname, writable_pkg, caller);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to insert badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(s)", pkgname);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	ret = send_notify(body, "insert_badge_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

/* delete_badge */
int badge_delete(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	GVariant *body = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &caller);
	pkgname = string_get(pkgname);
	caller = string_get(caller);

	if (pkgname != NULL && caller != NULL) {
		ret = badge_db_delete(pkgname, pkgname);
	} else {
		return BADGE_ERROR_INVALID_PARAMETER;
	}

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to delete badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(s)", pkgname);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "delete_badge_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_badge_count */
int badge_set_badge_count(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	int count = 0;
	GVariant *body = NULL;

	g_variant_get(parameters, "(ssi)", &pkgname, &caller, &count);
	pkgname = string_get(pkgname);
	caller = string_get(caller);

	if (pkgname != NULL && caller != NULL)
		ret = badge_db_set_count(pkgname, caller, count);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(si)", pkgname, count);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "set_badge_count_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_badge_count */
int badge_get_badge_count(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	int count = 0;

	g_variant_get(parameters, "(s)", &pkgname);
	pkgname = string_get(pkgname);

	if (pkgname != NULL)
		ret =  badge_db_get_count(pkgname, &count);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to get badge count :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", count);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("badge_get_badge_count service done");
	return ret;
}

/* set_disp_option */
int badge_set_display_option(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	int is_display = 0;
	GVariant *body = NULL;

	g_variant_get(parameters, "(ssi)", &pkgname, &caller, &is_display);
	pkgname = string_get(pkgname);
	caller = string_get(caller);

	if (pkgname != NULL && caller != NULL)
		ret = badge_db_set_display_option(pkgname, caller, is_display);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set display option :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(is)", pkgname, is_display);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "set_disp_option_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_disp_option */
int badge_get_display_option(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	int is_display = 0;

	g_variant_get(parameters, "(s)", &pkgname);
	pkgname = string_get(pkgname);

	if (pkgname != NULL)
		ret = badge_db_get_display_option(pkgname, &is_display);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set display option :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", is_display);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_noti_property */
int badge_set_setting_property(GVariant *parameters, GVariant **reply_body)
{
	int ret = 0;
	int is_display = 0;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;
	GVariant *body = NULL;

	g_variant_get(parameters, "(sss)", &pkgname, &property, &value);
	pkgname = string_get(pkgname);
	property = string_get(property);
	value = string_get(value);

	if (pkgname != NULL && property != NULL && value != NULL)
		ret = badge_setting_db_set(pkgname, property, value);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to setting db set :%d\n", ret);
		return ret;
	}

	if (ret == BADGE_ERROR_NONE) {
		if (strcmp(property, "OPT_BADGE") == 0) {
			if (strcmp(value, "ON") == 0)
				is_display = 1;
			else
				is_display = 0;

			body = g_variant_new("(si)", pkgname, is_display);
			if (body == NULL) {
				ErrPrint("cannot make gvariant to noti");
				return BADGE_ERROR_OUT_OF_MEMORY;
			}
			ret = send_notify(body, "set_disp_option_notify", BADGE_SERVICE);
			if (ret != BADGE_ERROR_NONE) {
				ErrPrint("failed to send notify:%d\n", ret);
				return ret;
			}
		}
	} else {
		ErrPrint("failed to set noti property:%d\n", ret);
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_noti_property */
int badge_get_setting_property(GVariant *parameters, GVariant **reply_body)
{
	int ret = 0;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &property);
	pkgname = string_get(pkgname);
	property = string_get(property);

	if (pkgname != NULL && property != NULL)
		ret = badge_setting_db_get(pkgname, property, &value);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to setting db get :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(s)", value);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/*!
 * MAIN THREAD
 * Do not try to do anyother operation in these functions
 */
HAPI int badge_service_init(void)
{
	return BADGE_ERROR_NONE;
}

HAPI int badge_service_fini(void)
{
	return BADGE_ERROR_NONE;
}

/* End of a file */
