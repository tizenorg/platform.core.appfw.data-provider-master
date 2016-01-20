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
static GList *notification_monitoring_list = NULL;
static GList *badge_monitoring_list = NULL;
static GList *shortcut_monitoring_list = NULL;

typedef struct monitoring_info {
	int watcher_id;
	char *bus_name;
	service_type type;
} monitoring_info_s;

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

int send_notify(GVariant *body, char *cmd, service_type type)
{
	GError *err = NULL;
	GList *target_list;
	char *target_bus_name = NULL;
	GDBusConnection *conn = service_common_get_connection();
	GList *monitoring_app_list = NULL;
	char *interface_name = NULL;

	if(conn == NULL)
		return SERVICE_COMMON_ERROR_IO_ERROR;

	if (type == NOTIFICATION_SERVICE) {

		monitoring_app_list = notification_monitoring_list;
		interface_name = PROVIDER_NOTI_INTERFACE_NAME;

	} else if (type == BADGE_SERVICE) {

		monitoring_app_list = badge_monitoring_list;
		interface_name = PROVIDER_BADGE_INTERFACE_NAME;

	} else if (type == SHORTCUT_SERVICE) {

		monitoring_app_list = shortcut_monitoring_list;
		interface_name = PROVIDER_SHORTCUT_INTERFACE_NAME;
	}

	target_list = g_list_first(monitoring_app_list);
	for (; target_list != NULL; target_list = target_list->next) {
		err = NULL;
		target_bus_name = target_list->data;

		DbgPrint("emit signal to : %s", target_bus_name);
		if (g_dbus_connection_emit_signal(conn,
					target_bus_name,
					PROVIDER_OBJECT_PATH,
					interface_name,
					cmd,
					g_variant_ref(body),
					&err) == FALSE) {

			ErrPrint("g_dbus_connection_emit_signal() is failed");
			if (err != NULL) {
				ErrPrint("g_dbus_connection_emit_signal() err : %s",
						err->message);
				g_error_free(err);
			}
			return SERVICE_COMMON_ERROR_IO_ERROR;
		}
		DbgPrint("signal send done: %s", target_bus_name);
	}

	if (body != NULL)
		g_variant_unref(body);

	DbgPrint("provider _send_notify cmd %s done", cmd);
	return SERVICE_COMMON_ERROR_NONE;
}

/* register service */
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

	if (info->type == NOTIFICATION_SERVICE)
		notification_monitoring_list = g_list_remove(notification_monitoring_list, info->bus_name);
	else if (info->type == BADGE_SERVICE)
		badge_monitoring_list = g_list_remove(badge_monitoring_list, info->bus_name);
	else if (info->type == SHORTCUT_SERVICE)
		shortcut_monitoring_list = g_list_remove(shortcut_monitoring_list, info->bus_name);

	g_bus_unwatch_name(info->watcher_id);
	free(info);
}

static int _monitoring_app_list_compare_cb(gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

static void _server_register(GVariant *parameters, GVariant **reply_body, const gchar *sender, service_type type)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
	GList *added_list = NULL;
	char *bus_name = strdup(sender);
	monitoring_info_s *m_info = NULL;

	if (bus_name == NULL) {

		ret = SERVICE_COMMON_ERROR_IO_ERROR;

	} else {

		if (type == NOTIFICATION_SERVICE) {

			added_list = g_list_find_custom(notification_monitoring_list, sender,
					(GCompareFunc)_monitoring_app_list_compare_cb);
			if (added_list == NULL) {
				notification_monitoring_list = g_list_append(notification_monitoring_list, bus_name);
				DbgPrint("_server_register_service : register success sender is %s , length : %d",
						sender, g_list_length(notification_monitoring_list));

				m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
				if (m_info == NULL) {
					ErrPrint("Can not alloc monitoring_info_s");
					ret = SERVICE_COMMON_ERROR_IO_ERROR;
				}
			} else {
				ErrPrint("_server_register_service : register sender %s already exist", sender);
			}

		} else if (type == BADGE_SERVICE) {

			added_list = g_list_find_custom(badge_monitoring_list, sender,
					(GCompareFunc)_monitoring_app_list_compare_cb);
			if (added_list == NULL) {
				badge_monitoring_list = g_list_append(badge_monitoring_list, bus_name);
				DbgPrint("_server_register_service : register success sender is %s , length : %d",
						sender, g_list_length(badge_monitoring_list));

				m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
				if (m_info == NULL) {
					ErrPrint("Can not alloc monitoring_info_s");
					ret = SERVICE_COMMON_ERROR_IO_ERROR;
				}
			} else {
				ErrPrint("_server_register_service : register sender %s already exist", sender);
			}

		} else if (type == SHORTCUT_SERVICE) {

			added_list = g_list_find_custom(shortcut_monitoring_list, sender,
					(GCompareFunc)_monitoring_app_list_compare_cb);
			if (added_list == NULL) {
				shortcut_monitoring_list = g_list_append(shortcut_monitoring_list, bus_name);
				DbgPrint("_server_register_service : register success sender is %s , length : %d",
						sender, g_list_length(shortcut_monitoring_list));

				m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
				if (m_info == NULL) {
					ErrPrint("Can not alloc monitoring_info_s");
					ret = SERVICE_COMMON_ERROR_IO_ERROR;
				}
			} else {
				ErrPrint("_server_register_service : register sender %s already exist", sender);
			}

		}
	}

	if (ret == SERVICE_COMMON_ERROR_NONE && m_info != NULL) {
		m_info->type = type;
		m_info->bus_name = bus_name;
		m_info->watcher_id = g_bus_watch_name_on_connection(
				_gdbus_conn,
				bus_name,
				G_BUS_NAME_WATCHER_FLAGS_NONE,
				_on_name_appeared,
				_on_name_vanished,
				m_info,
				NULL);
		if (m_info->watcher_id == 0) {
			ErrPrint("fail to watch name");
			ret = SERVICE_COMMON_ERROR_IO_ERROR;
		} else {
			DbgPrint("watch on %s success", bus_name);
		}
	}

	*reply_body = g_variant_new("(i)", ret);
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
		_server_register(parameters, &reply_body, sender, NOTIFICATION_SERVICE);
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
		_server_register(parameters, &reply_body, sender, BADGE_SERVICE);
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
		_server_register(parameters, &reply_body, sender, SHORTCUT_SERVICE);
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
	int result = SERVICE_COMMON_ERROR_NONE;
	GDBusNodeInfo *introspection_data = NULL;
	int noti_registration_id = 0;
	int badge_registration_id = 0;
	int shortcut_registration_id = 0;
	static gchar introspection_prefix[] =
			"  <interface name='";
	static gchar introspection_noti_postfix[] =
			"'>"
			"        <method name='noti_service_register'>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='add_noti'>"
			"          <arg type='v' name='noti' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
			"        </method>"

			"        <method name='update_noti'>"
			"          <arg type='v' name='noti' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='i' name='priv_id' direction='out'/>"
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

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"
			"        <method name='load_noti_by_priv_id'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='v' name='noti' direction='out'/>"
			"        </method>"
			"        <method name='load_noti_grouping_list'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='a(v)' name='noti_list' direction='out'/>"
			"        </method>"
			"        <method name='load_noti_detail_list'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='a(v)' name='noti_list' direction='out'/>"
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
			"        <method name='get_noti_count'>"
			"          <arg type='i' name='type' direction='in'/>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='group_id' direction='in'/>"
			"          <arg type='i' name='priv_id' direction='in'/>"

			"          <arg type='i' name='ret' direction='out'/>"
			"          <arg type='i' name='ret_count' direction='out'/>"
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

			"        <method name='get_setting_array'>"
			"          <arg type='i' name='result' direction='out'/>"
			"          <arg type='i' name='setting_cnt' direction='out'/>"
			"          <arg type='a(v)' name='setting_arr' direction='out'/>"
			"        </method>"

			"        <method name='get_setting_by_package_name'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='result' direction='out'/>"
			"          <arg type='v' name='setting' direction='out'/>"
			"        </method>"

			"        <method name='load_system_setting'>"
			"          <arg type='i' name='result' direction='out'/>"
			"          <arg type='v' name='setting' direction='out'/>"
			"        </method>"


			"        <method name='post_toast'>"
			"        </method>"
			"  </interface>";


	static gchar introspection_badge_postfix[] =
			"'>"
			"        <method name='badge_service_register'>"
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
			"        <method name='get_badge_count'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='count' direction='out'/>"
			"        </method>"
			"        <method name='set_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='is_display' direction='in'/>"
			"          <arg type='i' name='ret' direction='out'/>"
			"        </method>"
			"        <method name='get_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='is_display' direction='out'/>"
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
			"        <method name='shortcut_service_register'>"
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
		result = SERVICE_COMMON_ERROR_IO_ERROR;
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
		result = SERVICE_COMMON_ERROR_IO_ERROR;
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
		result = SERVICE_COMMON_ERROR_IO_ERROR;
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
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		goto out;
	}

	badge_registration_id = g_dbus_connection_register_object(_gdbus_conn,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[1],
			&_badge_interface_vtable, NULL, NULL, NULL);
	DbgPrint("badge_registration_id %d", badge_registration_id);
	if (badge_registration_id == 0) {
		ErrPrint("Failed to g_dbus_connection_register_object");
		result = SERVICE_COMMON_ERROR_IO_ERROR;
		goto out;
	}

	shortcut_registration_id = g_dbus_connection_register_object(_gdbus_conn,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[2],
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
	if (introspection_xml)
		free(introspection_xml);
	return result;
}

static int _dbus_init(void)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
	GError *error = NULL;

	_gdbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (_gdbus_conn == NULL) {
		if (error != NULL) {
			ErrPrint("Failed to get dbus [%s]", error->message);
			g_error_free(error);
		}
		return SERVICE_COMMON_ERROR_IO_ERROR;
	}

	return ret;

}

int service_common_dbus_init() {
	int ret = SERVICE_COMMON_ERROR_NONE;
	if (_gdbus_conn == NULL) {
		ret = _dbus_init();
		if( ret != SERVICE_COMMON_ERROR_NONE)
			return ret;
		ret = _register_dbus_interface();
	}
	return ret;
}

GDBusConnection *service_common_get_connection() {
	service_common_dbus_init();
	return _gdbus_conn;
}
