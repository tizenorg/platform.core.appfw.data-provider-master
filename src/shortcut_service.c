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

#define PROVIDER_SHORTCUT_INTERFACE_NAME "org.tizen.data_provider_shortcut_service"

static GList *_monitoring_list = NULL;

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
		g_bus_unwatch_name(info->watcher_id);

		if (info->bus_name) {
			_monitoring_list = g_list_remove(_monitoring_list, info->bus_name);
			free(info->bus_name);
		}
		free(info);
	}

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
	int ret = SHORTCUT_ERROR_NOT_SUPPORTED;

	if (g_strcmp0(method_name, "shortcut_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender,
		 _on_name_appeared, _on_name_vanished, &_monitoring_list);
	else if (g_strcmp0(method_name, "add_shortcut") == 0)
		ret = shortcut_add(parameters, &reply_body);
	else if (g_strcmp0(method_name, "add_shortcut_widget") == 0)
		ret = shortcut_add_widget(parameters, &reply_body);

	if (ret == SHORTCUT_ERROR_NONE) {
		DbgPrint("shortcut service success : %d", ret);
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

int shortcut_register_dbus_interface()
{
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

	return service_common_register_dbus_interface(introspection_xml, _shortcut_interface_vtable);
}

/* add_shortcut */
int shortcut_add(GVariant *parameters, GVariant **reply_body)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	ret = send_notify(parameters, "add_shortcut_notify", _monitoring_list, PROVIDER_SHORTCUT_INTERFACE_NAME);
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

	ret = send_notify(parameters, "add_shortcut_widget_notify", _monitoring_list, PROVIDER_SHORTCUT_INTERFACE_NAME);
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
	int result;

	result = shortcut_register_dbus_interface();
	if(result != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("shortcut register dbus fail %d", result);
	}
	return result;
}

HAPI int shortcut_service_fini(void)
{
	DbgPrint("Successfully Finalized\n");
	return SERVICE_COMMON_ERROR_NONE;
}

/* End of a file */
