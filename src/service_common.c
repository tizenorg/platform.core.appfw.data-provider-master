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
#include <gio/gio.h>
#include <dlog.h>
#include <cynara-client.h>
#include <cynara-session.h>
#include <cynara-creds-socket.h>
#include <notification.h>

#include "service_common.h"
#include "debug.h"

#include "notification_service.h"
#include "badge_service.h"
#include "shortcut_service.h"

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"
#define PROVIDER_NOTI_INTERFACE_NAME "org.tizen.data_provider_noti_service"
#define PROVIDER_BADGE_INTERFACE_NAME "org.tizen.data_provider_badge_service"
#define PROVIDER_SHORTCUT_INTERFACE_NAME "org.tizen.data_provider_shortcut_service"

#define DBUS_SERVICE_DBUS "org.freedesktop.DBus"
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"
#define DBUS_INTERFACE_DBUS "org.freedesktop.DBus"
static GDBusConnection *_gdbus_conn = NULL;

void print_noti(notification_h noti) {
	char *pkgname = NULL;
	char *text = NULL;
	char *content = NULL;
	const char *tag = NULL;

	notification_get_pkgname(noti, &pkgname);
	notification_get_text(noti, NOTIFICATION_TEXT_TYPE_TITLE, &text);
	notification_get_text(noti, NOTIFICATION_TEXT_TYPE_CONTENT, &content);
	notification_get_tag(noti, &tag);

	DbgPrint("provider print_noti  pkgname  = %s ", pkgname );
	DbgPrint("provider print_noti  title  = %s ", text );
	DbgPrint("provider print_noti  content  = %s ", content );
	DbgPrint("provider print_noti  tag  = %s ", tag );
}

char *string_get(char *string)
{
	if (string == NULL)
		return NULL;
	if (string[0] == '\0')
		return NULL;

	return string;
}

static void _noti_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	DbgPrint("notification method_name: %s", method_name);
	if (g_strcmp0(method_name, "service_register") == 0)
		notification_server_register(parameters, invocation);
	else if (g_strcmp0(method_name, "update_noti") == 0)
		notification_update_noti(parameters, invocation);
	else if (g_strcmp0(method_name, "add_noti") == 0)
		notification_add_noti(parameters, invocation);
	else if (g_strcmp0(method_name, "refresh_noti") == 0)
		notification_refresh_noti(parameters, invocation);
	else if (g_strcmp0(method_name, "del_noti_single") == 0)
		notification_del_noti_single(parameters, invocation);
	else if (g_strcmp0(method_name, "del_noti_multiple") == 0)
		notification_del_noti_multiple(parameters, invocation);
	else if (g_strcmp0(method_name, "set_noti_property") == 0)
		notification_set_noti_property(parameters, invocation);
	else if (g_strcmp0(method_name, "get_noti_property") == 0)
		notification_get_noti_property(parameters, invocation);
	else if (g_strcmp0(method_name, "update_noti_setting") == 0)
		notification_update_noti_setting(parameters, invocation);
	else if (g_strcmp0(method_name, "update_noti_sys_setting") == 0)
		notification_update_noti_sys_setting(parameters, invocation);
	else if (g_strcmp0(method_name, "load_noti_by_tag") == 0)
		notification_load_noti_by_tag(parameters, invocation);
}

static const GDBusInterfaceVTable _noti_interface_vtable = {
		_noti_dbus_method_call_handler,
		NULL,
		NULL
};

static void _badge_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	DbgPrint("badge method_name: %s", method_name);
	if (g_strcmp0(method_name, "service_register") == 0)
		badge_server_register(parameters, invocation);
	else if (g_strcmp0(method_name, "insert_badge") == 0)
		badge_insert(parameters, invocation);
	else if (g_strcmp0(method_name, "delete_badge") == 0)
		badge_delete(parameters, invocation);
	else if (g_strcmp0(method_name, "set_badge_count") == 0)
		badge_set_badge_count(parameters, invocation);
	else if (g_strcmp0(method_name, "set_disp_option") == 0)
		badge_set_display_option(parameters, invocation);
	else if (g_strcmp0(method_name, "set_noti_property") == 0)
		badge_set_setting_property(parameters, invocation);
	else if (g_strcmp0(method_name, "get_noti_property") == 0)
		badge_get_setting_property(parameters, invocation);

}

static const GDBusInterfaceVTable _badge_interface_vtable = {
		_badge_dbus_method_call_handler,
		NULL,
		NULL
};


static void _shortcut_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	DbgPrint("shortcut method_name: %s", method_name);
	if (g_strcmp0(method_name, "service_register") == 0)
		shortcut_server_register(parameters, invocation);
	else if (g_strcmp0(method_name, "add_shortcut") == 0)
		shortcut_add(parameters, invocation);
	else if (g_strcmp0(method_name, "add_shortcut_widget") == 0)
		shortcut_add_widget(parameters, invocation);
}

static const GDBusInterfaceVTable _shortcut_interface_vtable = {
		_shortcut_dbus_method_call_handler,
		NULL,
		NULL
};


static void _on_bus_acquired(GDBusConnection *connection,
		const gchar *name, gpointer user_data)
{
	ErrPrint("_on_bus_acquired : %s", name);
}

static void _on_name_acquired(GDBusConnection *connection,
		const gchar *name, gpointer user_data)
{
	ErrPrint("_on_name_acquired : %s", name);
}

static void _on_name_lost(GDBusConnection *connection,
		const gchar *name, gpointer user_data)
{
	ErrPrint("_on_name_lost : %s", name);
}

int _register_dbus_interface()
{
	int result = NOTIFICATION_ERROR_NONE;
	GDBusNodeInfo *introspection_data = NULL;
	int noti_registration_id = 0;
	int badge_registration_id = 0;
	int shortcut_registration_id = 0;
	static gchar introspection_prefix[] =
			"  <interface name='";
	static gchar introspection_noti_postfix[] =
			"'>"
			"        <method name='service_register'>"
			"          <arg type='s' name='target_bus_name' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='add_noti'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='i' name='layout' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='internal_group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='s' name='caller_pkgname' direction='in'/>"
			"          <arg type='s' name='launch_pkgname' direction='in'/>"
			"          <arg type='s' name='args' direction='in'/>"
			"          <arg type='s' name='group_args' direction='in'/>"
			"          <arg type='s' name='b_execute_option' direction='in'/>"
			"          <arg type='s' name='b_service_responding' direction='in'/>"
			"          <arg type='s' name='b_service_single_launch' direction='in'/>"
			"          <arg type='s' name='b_service_multi_launch' direction='in'/>"
			"          <arg type='s' name='click_on_button1' direction='in'/>"
			"          <arg type='s' name='click_on_button2' direction='in'/>"
			"          <arg type='s' name='click_on_button3' direction='in'/>"
			"          <arg type='s' name='click_on_button4' direction='in'/>"
			"          <arg type='s' name='click_on_button5' direction='in'/>"
			"          <arg type='s' name='click_on_button6' direction='in'/>"
			"          <arg type='s' name='click_on_icon' direction='in'/>"
			"          <arg type='s' name='click_on_thumbnail' direction='in'/>"
			"          <arg type='s' name='domain' direction='in'/>"
			"          <arg type='s' name='dir' direction='in'/>"
			"          <arg type='s' name='b_text' direction='in'/>"
			"          <arg type='s' name='b_key' direction='in'/>"
			"          <arg type='s' name='b_format_args' direction='in'/>"
			"          <arg type='i' name='num_format_args' direction='in'/>"
			"          <arg type='s' name='b_image_path' direction='in'/>"
			"          <arg type='i' name='sound_type' direction='in'/>"
			"          <arg type='s' name='sound_path' direction='in'/>"
			"          <arg type='i' name='vibration_type' direction='in'/>"
			"          <arg type='s' name='vibration_path' direction='in'/>"
			"          <arg type='i' name='led_operation' direction='in'/>"
			"          <arg type='i' name='led_argb' direction='in'/>"
			"          <arg type='i' name='led_on_ms' direction='in'/>"
			"          <arg type='i' name='led_off_ms' direction='in'/>"
			"          <arg type='i' name='time' direction='in'/>"
			"          <arg type='i' name='insert_time' direction='in'/>"
			"          <arg type='i' name='flags_for_property' direction='in'/>"
			"          <arg type='i' name='display_applist' direction='in'/>"
			"          <arg type='d' name='progress_size' direction='in'/>"
			"          <arg type='d' name='progress_percentage' direction='in'/>"
			"          <arg type='s' name='app_icon_path' direction='in'/>"
			"          <arg type='s' name='app_name' direction='in'/>"
			"          <arg type='s' name='temp_title' direction='in'/>"
			"          <arg type='s' name='temp_content' direction='in'/>"
			"          <arg type='s' name='tag' direction='in'/>"
			"          <arg type='i' name='ongoing_flag' direction='in'/>"
			"          <arg type='i' name='auto_remove' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='update_noti'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='i' name='layout' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='internal_group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='s' name='caller_pkgname' direction='in'/>"
			"          <arg type='s' name='launch_pkgname' direction='in'/>"
			"          <arg type='s' name='args' direction='in'/>"
			"          <arg type='s' name='group_args' direction='in'/>"
			"          <arg type='s' name='b_execute_option' direction='in'/>"
			"          <arg type='s' name='b_service_responding' direction='in'/>"
			"          <arg type='s' name='b_service_single_launch' direction='in'/>"
			"          <arg type='s' name='b_service_multi_launch' direction='in'/>"
			"          <arg type='s' name='click_on_button1' direction='in'/>"
			"          <arg type='s' name='click_on_button2' direction='in'/>"
			"          <arg type='s' name='click_on_button3' direction='in'/>"
			"          <arg type='s' name='click_on_button4' direction='in'/>"
			"          <arg type='s' name='click_on_button5' direction='in'/>"
			"          <arg type='s' name='click_on_button6' direction='in'/>"
			"          <arg type='s' name='click_on_icon' direction='in'/>"
			"          <arg type='s' name='click_on_thumbnail' direction='in'/>"
			"          <arg type='s' name='domain' direction='in'/>"
			"          <arg type='s' name='dir' direction='in'/>"
			"          <arg type='s' name='b_text' direction='in'/>"
			"          <arg type='s' name='b_key' direction='in'/>"
			"          <arg type='s' name='b_format_args' direction='in'/>"
			"          <arg type='i' name='num_format_args' direction='in'/>"
			"          <arg type='s' name='b_image_path' direction='in'/>"
			"          <arg type='i' name='sound_type' direction='in'/>"
			"          <arg type='s' name='sound_path' direction='in'/>"
			"          <arg type='i' name='vibration_type' direction='in'/>"
			"          <arg type='s' name='vibration_path' direction='in'/>"
			"          <arg type='i' name='led_operation' direction='in'/>"
			"          <arg type='i' name='led_argb' direction='in'/>"
			"          <arg type='i' name='led_on_ms' direction='in'/>"
			"          <arg type='i' name='led_off_ms' direction='in'/>"
			"          <arg type='i' name='time' direction='in'/>"
			"          <arg type='i' name='insert_time' direction='in'/>"
			"          <arg type='i' name='flags_for_property' direction='in'/>"
			"          <arg type='i' name='display_applist' direction='in'/>"
			"          <arg type='d' name='progress_size' direction='in'/>"
			"          <arg type='d' name='progress_percentage' direction='in'/>"
			"          <arg type='s' name='app_icon_path' direction='in'/>"
			"          <arg type='s' name='app_name' direction='in'/>"
			"          <arg type='s' name='temp_title' direction='in'/>"
			"          <arg type='s' name='temp_content' direction='in'/>"
			"          <arg type='s' name='tag' direction='in'/>"
			"          <arg type='i' name='ongoing_flag' direction='in'/>"
			"          <arg type='i' name='auto_remove' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='refresh_noti'>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='del_noti_single'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"
			"        <method name='del_noti_multiple'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"
			"        <method name='load_noti_by_tag'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='tag' direction='in'/>"

			"          <arg type='i' name='type' direction='out'/>"
			"          <arg type='i' name='layout' direction='out'/>"
			"          <arg type='i' name='group_id' direction='out'/>"
			"          <arg type='i' name='internal_group_id' direction='out'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"          <arg type='s' name='caller_pkgname' direction='out'/>"
			"          <arg type='s' name='launch_pkgname' direction='out'/>"
			"          <arg type='s' name='args' direction='out'/>"
			"          <arg type='s' name='group_args' direction='out'/>"
			"          <arg type='s' name='b_execute_option' direction='out'/>"
			"          <arg type='s' name='b_service_responding' direction='out'/>"
			"          <arg type='s' name='b_service_single_launch' direction='out'/>"
			"          <arg type='s' name='b_service_multi_launch' direction='out'/>"
			"          <arg type='s' name='click_on_button1' direction='out'/>"
			"          <arg type='s' name='click_on_button2' direction='out'/>"
			"          <arg type='s' name='click_on_button3' direction='out'/>"
			"          <arg type='s' name='click_on_button4' direction='out'/>"
			"          <arg type='s' name='click_on_button5' direction='out'/>"
			"          <arg type='s' name='click_on_button6' direction='out'/>"
			"          <arg type='s' name='click_on_icon' direction='out'/>"
			"          <arg type='s' name='click_on_thumbnail' direction='out'/>"
			"          <arg type='s' name='domain' direction='out'/>"
			"          <arg type='s' name='dir' direction='out'/>"
			"          <arg type='s' name='b_text' direction='out'/>"
			"          <arg type='s' name='b_key' direction='out'/>"
			"          <arg type='s' name='b_format_args' direction='out'/>"
			"          <arg type='i' name='num_format_args' direction='out'/>"
			"          <arg type='s' name='b_image_path' direction='out'/>"
			"          <arg type='i' name='sound_type' direction='out'/>"
			"          <arg type='s' name='sound_path' direction='out'/>"
			"          <arg type='i' name='vibration_type' direction='out'/>"
			"          <arg type='s' name='vibration_path' direction='out'/>"
			"          <arg type='i' name='led_operation' direction='out'/>"
			"          <arg type='i' name='led_argb' direction='out'/>"
			"          <arg type='i' name='led_on_ms' direction='out'/>"
			"          <arg type='i' name='led_off_ms' direction='out'/>"
			"          <arg type='i' name='time' direction='out'/>"
			"          <arg type='i' name='insert_time' direction='out'/>"
			"          <arg type='i' name='flags_for_property' direction='out'/>"
			"          <arg type='i' name='display_applist' direction='out'/>"
			"          <arg type='d' name='progress_size' direction='out'/>"
			"          <arg type='d' name='progress_percentage' direction='out'/>"
			"          <arg type='s' name='app_icon_path' direction='out'/>"
			"          <arg type='s' name='app_name' direction='out'/>"
			"          <arg type='s' name='temp_title' direction='out'/>"
			"          <arg type='s' name='temp_content' direction='out'/>"
			"          <arg type='s' name='tag' direction='out'/>"
			"          <arg type='i' name='ongoing_flag' direction='out'/>"
			"          <arg type='i' name='auto_remove' direction='out'/>"
			"        </method>"
			"        <method name='set_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='get_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='s' name='ret_value' direction='out'/>"
			"        </method>"
			"        <method name='update_noti_setting'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='allow_to_notify' direction='in'/>"
			"          <arg type='i' name='do_not_disturb_except' direction='in'/>"
			"          <arg type='i' name='visibility_class' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='update_noti_sys_setting'>"
			"          <arg type='i' name='do_not_disturb' direction='in'/>"
			"          <arg type='i' name='visibility_class' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='post_toast'>"
			"        </method>"
			"  </interface>";


	static gchar introspection_badge_postfix[] =
			"'>"
			"        <method name='service_register'>"
			"          <arg type='s' name='target_bus_name' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='insert_badge'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='writable_pkg' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='delete_badge'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='set_badge_count'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='set_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='is_display' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='set_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='get_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='out'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"  </interface>";


	static gchar introspection_shortcut_postfix[] =
			"'>"
			"        <method name='service_register'>"
			"          <arg type='s' name='target_bus_name' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='add_shortcut'>"
			"          <arg type='i' name='pid' direction='in'/>"
			"          <arg type='s' name='appid' direction='in'/>"
			"          <arg type='s' name='name' direction='in'/>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='s' name='uri' direction='in'/>"
			"          <arg type='s' name='icon' direction='in'/>"
			"          <arg type='i' name='allow_duplicate' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
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

			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"  </interface>";

	static gchar introspection_node_prefix[] = "<node>";
	static gchar introspection_node_postfix[] = "</node>";
	char *introspection_xml = NULL;
	int introspection_xml_len = 0;
	int owner_id = 0;
	GError *error = NULL;

	introspection_xml_len =
			strlen(introspection_node_prefix) +

			strlen(introspection_prefix) +
			strlen(PROVIDER_NOTI_INTERFACE_NAME) +
			strlen(introspection_noti_postfix) +

			strlen(introspection_prefix) +
			strlen(PROVIDER_BADGE_INTERFACE_NAME) +
			strlen(introspection_badge_postfix) +

			strlen(introspection_prefix) +
			strlen(PROVIDER_SHORTCUT_INTERFACE_NAME) +
			strlen(introspection_shortcut_postfix) +

			strlen(introspection_node_postfix) + 1;

	introspection_xml = (char *)calloc(introspection_xml_len, sizeof(char));
	if (!introspection_xml) {
		ErrPrint("out of memory");
		result = NOTIFICATION_ERROR_IO_ERROR;
		goto out;
	}

	owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
			PROVIDER_BUS_NAME,
			G_BUS_NAME_OWNER_FLAGS_NONE,
			_on_bus_acquired,
			_on_name_acquired,
			_on_name_lost,
			NULL, NULL);
	if (!owner_id) {
		ErrPrint("g_bus_own_name error");
		g_dbus_node_info_unref(introspection_data);
		result = NOTIFICATION_ERROR_IO_ERROR;
		goto out;
	}

	DbgPrint("Acquiring the own name : %d", owner_id);

	snprintf(introspection_xml, introspection_xml_len, "%s%s%s%s%s%s%s%s%s%s%s",
			introspection_node_prefix,
			introspection_prefix, PROVIDER_NOTI_INTERFACE_NAME, introspection_noti_postfix,
			introspection_prefix, PROVIDER_BADGE_INTERFACE_NAME, introspection_badge_postfix,
			introspection_prefix, PROVIDER_SHORTCUT_INTERFACE_NAME, introspection_shortcut_postfix,
			introspection_node_postfix);

	DbgPrint("introspection_xml : %s", introspection_xml);

	introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
	if (!introspection_data) {
		ErrPrint("g_dbus_node_info_new_for_xml() is failed.");
		result = NOTIFICATION_ERROR_IO_ERROR;
		if (error != NULL) {
			ErrPrint("g_dbus_node_info_new_for_xml error [%s]", error->message);
			g_error_free(error);
		}
		goto out;
	}

	noti_registration_id = g_dbus_connection_register_object(_gdbus_conn,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[0],
			&_noti_interface_vtable, NULL, NULL, NULL);
	DbgPrint("noti_registration_id %d", noti_registration_id);
	if (noti_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = NOTIFICATION_ERROR_IO_ERROR;
		goto out;
	}

	badge_registration_id = g_dbus_connection_register_object(_gdbus_conn,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[1],
			&_badge_interface_vtable, NULL, NULL, NULL);
	DbgPrint("badge_registration_id %d", badge_registration_id);
	if (badge_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = NOTIFICATION_ERROR_IO_ERROR;
		goto out;
	}

	shortcut_registration_id = g_dbus_connection_register_object(_gdbus_conn,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[2],
			&_shortcut_interface_vtable, NULL, NULL, NULL);
	DbgPrint("shortcut_registration_id %d", shortcut_registration_id);
	if (shortcut_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = NOTIFICATION_ERROR_IO_ERROR;
		goto out;
	}

out:
	if (introspection_data)
		g_dbus_node_info_unref(introspection_data);
	if (introspection_xml)
		free(introspection_xml);

	return result;
}

static int _dbus_init(void)
{
	bool ret = false;
	GError *error = NULL;

	_gdbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (_gdbus_conn == NULL) {
		if (error != NULL) {
			ErrPrint("Failed to get dbus [%s]", error->message);
			g_error_free(error);
		}
		goto out;
	}
	ret = true;
out:
	if (!_gdbus_conn)
		g_object_unref(_gdbus_conn);

	return ret;

}

int service_common_dbus_init() {
	int ret = NOTIFICATION_ERROR_NONE;
	if (_gdbus_conn == NULL) {
		_dbus_init();
		ret = _register_dbus_interface();
	}
	return ret;
}

GDBusConnection *service_common_get_connection() {
	service_common_dbus_init();
	return _gdbus_conn;
}
