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
#include <shortcut.h>
#include "service_common.h"
#include "shortcut_service.h"
#include "debug.h"

#define SHORTCUT_IPC_OBJECT_PATH "/org/tizen/shortcut_service"

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"

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

static void _shortcut_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	/* TODO : sender authority(privilege) check */
	DbgPrint("shortcut method_name: %s", method_name);
	GVariant *reply_body = NULL;
	int ret = SHORTCUT_ERROR_NONE;

	if (g_strcmp0(method_name, "shortcut_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender, SHORTCUT_SERVICE);
	else if (g_strcmp0(method_name, "add_shortcut") == 0)
		ret = shortcut_add(parameters, &reply_body);
	else if (g_strcmp0(method_name, "add_shortcut_widget") == 0)
		ret = shortcut_add_widget(parameters, &reply_body);

	if (ret == SHORTCUT_ERROR_NONE) {
		DbgPrint("badge service success : %d", ret);
		g_dbus_method_invocation_return_value(
				invocation, reply_body);
	} else {
		DbgPrint("shortcut service fail : %d", ret);
		g_dbus_method_invocation_return_error(
				invocation,
				SHORTCUT_ERROR,
				ret,
				"shortcut service error");
	}
}

static const GDBusInterfaceVTable _shortcut_interface_vtable = {
		_shortcut_dbus_method_call_handler,
		NULL,
		NULL
};

int shortcut_register_dbus_interface(GDBusConnection *connection)
{
	int result = SERVICE_COMMON_ERROR_NONE;
	GDBusNodeInfo *introspection_data = NULL;
	int shortcut_registration_id = 0;

	static gchar introspection_xml[] =
			"  <node>"
			"  <interface name='org.tizen.data_provider_shortcut_service'>"
			"        <method name='shortcut_service_register'>"
			"        </method>"
			"        <method name='add_shortcut'>"
			"          <arg type='i' name='pid' direction='in'/>"
			"          <arg type='s' name='appid' direction='in'/>"
			"          <arg type='s' name='name' direction='in'/>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='s' name='uri' direction='in'/>"
			"          <arg type='s' name='icon' direction='in'/>"
			"          <arg type='i' name='allow_duplicate' direction='in'/>"
			"        </method>"

			"        <method name='add_shortcut_widget'>"
			"          <arg type='i' name='pid' direction='in'/>"
			"          <arg type='s' name='widget_id' direction='in'/>"
			"          <arg type='s' name='name' direction='in'/>"
			"          <arg type='i' name='size' direction='in'/>"
			"          <arg type='s' name='uri' direction='in'/>"
			"          <arg type='s' name='icon' direction='in'/>"
			"          <arg type='d' name='period' direction='in'/>"
			"          <arg type='i' name='allow_duplicate' direction='in'/>"
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

	shortcut_registration_id = g_dbus_connection_register_object(connection,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[0],
			&_shortcut_interface_vtable, NULL, NULL, NULL);
	DbgPrint("shortcut_registration_id %d", shortcut_registration_id);
	if (shortcut_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		goto out;
	}

out:
	if (introspection_data)
		g_dbus_node_info_unref(introspection_data);

	return result;
}

/* add_shortcut */
int shortcut_add(GVariant *parameters, GVariant **reply_body)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	ret = send_notify(parameters, "add_shortcut_notify", SHORTCUT_SERVICE);
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("Cannot make reply body");
		return SHORTCUT_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

/* add_shortcut_widget */
int shortcut_add_widget(GVariant *parameters, GVariant **reply_body)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	ret = send_notify(parameters, "add_shortcut_widget_notify", SHORTCUT_SERVICE);
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("Cannot make reply body");
		return SHORTCUT_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

/*!
 * MAIN THREAD
 * Do not try to do anyother operation in these functions
 */
HAPI int shortcut_service_init(void)
{
	DbgPrint("Successfully initiated\n");
	return SERVICE_COMMON_ERROR_NONE;
}

HAPI int shortcut_service_fini(void)
{
	DbgPrint("Successfully Finalized\n");
	return SERVICE_COMMON_ERROR_NONE;
}

/* End of a file */
