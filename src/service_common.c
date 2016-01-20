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

int service_register(GVariant *parameters, GVariant **reply_body, const gchar *sender, service_type type)
{
	GList *added_list = NULL;
	char *bus_name = strdup(sender);
	monitoring_info_s *m_info = NULL;

	if (bus_name == NULL) {

		return SERVICE_COMMON_ERROR_IO_ERROR;

	} else {

		if (type == NOTIFICATION_SERVICE) {

			added_list = g_list_find_custom(notification_monitoring_list, sender,
					(GCompareFunc)_monitoring_app_list_compare_cb);
			if (added_list == NULL) {
				notification_monitoring_list = g_list_append(notification_monitoring_list, bus_name);
				DbgPrint("service_register : register success sender is %s , length : %d",
						sender, g_list_length(notification_monitoring_list));

				m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
				if (m_info == NULL) {
					ErrPrint("Can not alloc monitoring_info_s");
					return SERVICE_COMMON_ERROR_IO_ERROR;
				}
			} else {
				ErrPrint("service_register : register sender %s already exist", sender);
			}

		} else if (type == BADGE_SERVICE) {

			added_list = g_list_find_custom(badge_monitoring_list, sender,
					(GCompareFunc)_monitoring_app_list_compare_cb);
			if (added_list == NULL) {
				badge_monitoring_list = g_list_append(badge_monitoring_list, bus_name);
				DbgPrint("service_register : register success sender is %s , length : %d",
						sender, g_list_length(badge_monitoring_list));

				m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
				if (m_info == NULL) {
					ErrPrint("Can not alloc monitoring_info_s");
					return SERVICE_COMMON_ERROR_IO_ERROR;
				}
			} else {
				ErrPrint("service_register : register sender %s already exist", sender);
			}

		} else if (type == SHORTCUT_SERVICE) {

			added_list = g_list_find_custom(shortcut_monitoring_list, sender,
					(GCompareFunc)_monitoring_app_list_compare_cb);
			if (added_list == NULL) {
				shortcut_monitoring_list = g_list_append(shortcut_monitoring_list, bus_name);
				DbgPrint("service_register : register success sender is %s , length : %d",
						sender, g_list_length(shortcut_monitoring_list));

				m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
				if (m_info == NULL) {
					ErrPrint("Can not alloc monitoring_info_s");
					return SERVICE_COMMON_ERROR_IO_ERROR;
				}
			} else {
				ErrPrint("service_register : register sender %s already exist", sender);
			}
		}
	}

	if (m_info != NULL) {
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
			free(m_info);
			return SERVICE_COMMON_ERROR_IO_ERROR;
		} else {
			DbgPrint("watch on %s success", bus_name);
		}
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return SERVICE_COMMON_ERROR_OUT_OF_MEMORY;
	}

	return SERVICE_COMMON_ERROR_NONE;
}

int _register_dbus_interface()
{
	int result = SERVICE_COMMON_ERROR_NONE;

	result = notification_register_dbus_interface(_gdbus_conn);
	if(result != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("notification register dbus fail %d", result);
		return result;
	}

	badge_register_dbus_interface(_gdbus_conn);
	if(result != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("badge register dbus fail %d", result);
		return result;
	}

	shortcut_register_dbus_interface(_gdbus_conn);
	if(result != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("shortcut register dbus fail %d", result);
		return result;
	}

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
		if(ret != SERVICE_COMMON_ERROR_NONE)
			return ret;
		ret = _register_dbus_interface();
	}
	return ret;
}

GDBusConnection *service_common_get_connection() {
	service_common_dbus_init();
	return _gdbus_conn;
}
