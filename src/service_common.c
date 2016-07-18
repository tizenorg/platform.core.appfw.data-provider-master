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
#ifndef WEARABLE
#include "shortcut_service.h"
#endif

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"

static GDBusConnection *_gdbus_conn = NULL;

void print_noti(notification_h noti)
{
	char *pkgname = NULL;
	char *text = NULL;
	char *content = NULL;
	const char *tag = NULL;
	const char *vibration_path = NULL;
	notification_vibration_type_e type;

	notification_get_pkgname(noti, &pkgname);
	notification_get_text(noti, NOTIFICATION_TEXT_TYPE_TITLE, &text);
	notification_get_text(noti, NOTIFICATION_TEXT_TYPE_CONTENT, &content);
	notification_get_tag(noti, &tag);
	notification_get_vibration(noti, &type, &vibration_path);

	DbgPrint("provider print_noti  pkgname  = %s", pkgname);
	DbgPrint("provider print_noti  title  = %s", text);
	DbgPrint("provider print_noti  content  = %s", content);
	DbgPrint("provider print_noti  tag  = %s", tag);
	DbgPrint("provider print_noti  vibration_path  = %s %d", vibration_path, type);
}

static void _free_monitoring_info(gpointer data)
{
	monitoring_info_s *info = (monitoring_info_s *)data;
	if (info) {
		free(info->bus_name);
		g_bus_unwatch_name(info->watcher_id);
		free(info);
	}
}

void free_monitoring_list(gpointer data)
{
	GList *monitoring_list = (GList *)data;
	g_list_free_full(monitoring_list, _free_monitoring_info);
}

uid_t get_sender_uid(const char *sender_name)
{
	GDBusMessage *msg = NULL;
	GDBusMessage *reply = NULL;
	GError *err = NULL;
	GVariant *body;
	uid_t uid = 0;

	msg = g_dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "GetConnectionUnixUser");
	if (!msg) {
		LOGE("Can't allocate new method call");
		goto out;
	}

	g_dbus_message_set_body(msg, g_variant_new("(s)", sender_name));
	reply = g_dbus_connection_send_message_with_reply_sync(_gdbus_conn, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err);

	if (!reply) {
		if (err != NULL) {
			LOGE("Failed to get uid [%s]", err->message);
			g_error_free(err);
		}
		goto out;
	}

	body = g_dbus_message_get_body(reply);
	g_variant_get(body, "(u)", &uid);

out:
	if (msg)
		g_object_unref(msg);
	if (reply)
		g_object_unref(reply);

	return uid;
}

int send_notify(GVariant *body, char *cmd, GList *monitoring_app_list, char *interface_name)
{
	GError *err = NULL;
	GList *target_list;
	char *target_bus_name;

	target_list = g_list_first(monitoring_app_list);
	for (; target_list != NULL; target_list = target_list->next) {
		err = NULL;
		target_bus_name = target_list->data;

		if (g_variant_is_floating(body))
			g_variant_ref(body);

		DbgPrint("emit signal to : %s", target_bus_name);
		if (g_dbus_connection_emit_signal(_gdbus_conn,
					target_bus_name,
					PROVIDER_OBJECT_PATH,
					interface_name,
					cmd,
					body,
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

	DbgPrint("provider _send_notify cmd %s done", cmd);
	return SERVICE_COMMON_ERROR_NONE;
}

/* register service */

static int _monitoring_app_list_compare_cb(gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

int service_register(GVariant *parameters, GVariant **reply_body, const gchar *sender,
		GBusNameAppearedCallback name_appeared_handler,
		GBusNameVanishedCallback name_vanished_handler,
		GHashTable **monitoring_hash,
		uid_t uid)
{
	GList *added_list = NULL;
	const char *bus_name = sender;
	monitoring_info_s *m_info = NULL;
	uid_t request_uid = 0;
	GList *monitoring_list = NULL;
	int *hash_key;

	if (sender == NULL)
		return SERVICE_COMMON_ERROR_IO_ERROR;

	g_variant_get(parameters, "(i)", &request_uid);

	if (uid > NORMAL_UID_BASE && uid != request_uid)
		return SERVICE_COMMON_ERROR_IO_ERROR;

	DbgPrint("service_register : uid %d , request_uid %d", uid, request_uid);
	monitoring_list = (GList *)g_hash_table_lookup(*monitoring_hash, &request_uid);
	added_list = g_list_find_custom(monitoring_list, bus_name,
			(GCompareFunc)_monitoring_app_list_compare_cb);

	if (added_list == NULL) {
		DbgPrint("add new sender to list");
		m_info = (monitoring_info_s *)calloc(1, sizeof(monitoring_info_s));
		if (m_info == NULL) {
			ErrPrint("Can not alloc monitoring_info_s");
			return SERVICE_COMMON_ERROR_IO_ERROR;
		}

		m_info->bus_name = strdup(bus_name);
		m_info->watcher_id = g_bus_watch_name_on_connection(
				_gdbus_conn,
				bus_name,
				G_BUS_NAME_WATCHER_FLAGS_NONE,
				name_appeared_handler,
				name_vanished_handler,
				m_info,
				NULL);
		if (m_info->watcher_id == 0) {
			ErrPrint("fail to watch name");
			free(m_info->bus_name);
			free(m_info);
			return SERVICE_COMMON_ERROR_IO_ERROR;
		} else {
			DbgPrint("watch on %s success", bus_name);
		}

		monitoring_list = g_list_append(monitoring_list, strdup(bus_name));
		DbgPrint("service_register : register success sender is %s , length : %d",
				sender, g_list_length(monitoring_list));
		if (g_hash_table_lookup(*monitoring_hash, &request_uid) == NULL) {
			hash_key = (int *)calloc(1, sizeof(int));
			*hash_key = request_uid;
			g_hash_table_insert(*monitoring_hash, hash_key, monitoring_list);
		}

	} else {
		ErrPrint("service_register : register sender %s already exist", sender);
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		free(m_info->bus_name);
		free(m_info);
		monitoring_list = g_list_remove(monitoring_list, bus_name);
		ErrPrint("cannot make reply_body");
		return SERVICE_COMMON_ERROR_OUT_OF_MEMORY;
	}
	return SERVICE_COMMON_ERROR_NONE;
}

static int _dbus_init(void)
{
	GError *error = NULL;

	if (_gdbus_conn == NULL) {
		_gdbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
		if (_gdbus_conn == NULL) {
			if (error != NULL) {
				ErrPrint("Failed to get dbus [%s]", error->message);
				g_error_free(error);
			}
			return SERVICE_COMMON_ERROR_IO_ERROR;
		}
	}

	return SERVICE_COMMON_ERROR_NONE;
}

int service_common_register_dbus_interface(char *introspection_xml, GDBusInterfaceVTable interface_vtable)
{
	int result;
	int owner_id, noti_registration_id;
	GError *error = NULL;
	GDBusNodeInfo *introspection_data = NULL;

	result = _dbus_init();
	if (result != SERVICE_COMMON_ERROR_NONE) {
			ErrPrint("Can't init dbus %d", result);
			result = SERVICE_COMMON_ERROR_IO_ERROR;
			goto out;
	}

	owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
			PROVIDER_BUS_NAME,
			G_BUS_NAME_OWNER_FLAGS_NONE,
			NULL,
			NULL,
			NULL,
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

	noti_registration_id = g_dbus_connection_register_object(_gdbus_conn,
			PROVIDER_OBJECT_PATH, introspection_data->interfaces[0],
			&interface_vtable, NULL, NULL, NULL);

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
