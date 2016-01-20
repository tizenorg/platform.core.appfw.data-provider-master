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
#include "debug.h"
#define SHORTCUT_IPC_OBJECT_PATH "/org/tizen/shortcut_service"

static GList *monitoring_app_list;

static int _send_notify(GVariant *body, char *cmd)
{
	DbgPrint("_send_notify");
	GError *err = NULL;
	GDBusMessage *msg = NULL;
	GList *target_list;
	char *target_bus_name = NULL;

	target_list = monitoring_app_list;
	for (; target_list != NULL; target_list = target_list->next) {
		target_bus_name = target_list->data;
		msg = g_dbus_message_new_method_call(
				target_bus_name,
				SHORTCUT_IPC_OBJECT_PATH,
				target_bus_name,
				cmd);
		if (!msg) {
			ErrPrint("Can't allocate new method call");
			return SERVICE_COMMON_ERROR_OUT_OF_MEMORY;
		}

		g_dbus_message_set_body(msg, body);
		g_dbus_connection_send_message(service_common_get_connection(), msg, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &err);
		if (err != NULL) {
			ErrPrint("No reply. error = %s", err->message);
			g_error_free(err);
		}
	}
	ErrPrint("_send_notify %s done", cmd);
	return SERVICE_COMMON_ERROR_NONE;
}

/* register service */
static int _monitoring_app_list_compare_cb(gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

void shortcut_server_register(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
	char *bus_name = NULL;
	GList *added_list = NULL;

	g_variant_get(parameters, "(s)", &bus_name);
	if (bus_name != NULL) {
		added_list = g_list_find_custom(monitoring_app_list, bus_name,
		                                        (GCompareFunc)_monitoring_app_list_compare_cb);
		if (added_list == NULL) {
			monitoring_app_list = g_list_append(monitoring_app_list, bus_name);
			ErrPrint("_server_register_service : register success bus_name is %s", bus_name);
		} else {
			ErrPrint("_server_register_service : register bus_name %s already exist", bus_name);
		}

	} else {
		ErrPrint("_server_register_service : bus_name is NULL");
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;
	}
	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));
}

/* add_shortcut */
void shortcut_add(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	ret = _send_notify(parameters, "add_shortcut_notify");
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
}

/* add_shortcut_widget */
void shortcut_add_widget(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	ret = _send_notify(parameters, "add_shortcut_widget_notify");
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
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
