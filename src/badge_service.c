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

#include "service_common.h"
#include "debug.h"

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"
#define PROVIDER_BADGE_INTERFACE_NAME "org.tizen.data_provider_badge_service"

static GList *monitoring_app_list;
static int _send_notify(GVariant *body, char *cmd)
{
	GError *err = NULL;
	GList *target_list;
	char *target_bus_name = NULL;
	GDBusConnection *conn = service_common_get_connection();

	if(conn == NULL)
		return SERVICE_COMMON_ERROR_IO_ERROR;

	target_list = g_list_first(monitoring_app_list);
	for (; target_list != NULL; target_list = target_list->next) {
		target_bus_name = target_list->data;

		DbgPrint("emit signal to : %s", target_bus_name);
		if (g_dbus_connection_emit_signal(conn,
					target_bus_name,
					PROVIDER_OBJECT_PATH,
					PROVIDER_BADGE_INTERFACE_NAME,
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
	}
	DbgPrint("_send_notify cmd %s done", cmd);
	return SERVICE_COMMON_ERROR_NONE;
}

/* register service */
static int _monitoring_app_list_compare_cb(gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

void badge_server_register(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
	char *bus_name = NULL;
	GList *added_list = NULL;

	g_variant_get(parameters, "(s)", &bus_name);
	if (bus_name != NULL) {
		added_list = g_list_find_custom(monitoring_app_list, bus_name,
		                                        (GCompareFunc)_monitoring_app_list_compare_cb);
		if (added_list == NULL) {
			monitoring_app_list = g_list_append(monitoring_app_list, strdup(bus_name));
			DbgPrint("_server_register_service : register success bus_name is %s, length : %d",
					bus_name, g_list_length(monitoring_app_list));
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

/* insert_badge */
void badge_insert(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
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
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	body = g_variant_new("(is)", ret, pkgname);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "insert_badge_notify");
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
}

/* delete_badge */
void badge_delete(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	GVariant *body = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &caller);
	pkgname = string_get(pkgname);
	caller = string_get(caller);

	if (pkgname != NULL && caller != NULL) {
		ret = badge_db_delete(pkgname, pkgname);
//		if (service_check_privilege_by_socket_fd(tcb_svc_ctx(tcb), tcb_fd(tcb), "http://tizen.org/privilege/notification") == 1)
//			ret = badge_db_delete(pkgname, pkgname);
//		else
//			ret = badge_db_delete(pkgname, caller);
	} else {
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;
	}

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	body = g_variant_new("(is)", ret, pkgname);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "delete_badge_notify");
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
}

/* set_badge_count */
void badge_set_badge_count(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
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
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	body = g_variant_new("(isi)", ret, pkgname, count);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "set_badge_count_notify");
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
}

/* set_disp_option */
void badge_set_display_option(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = SERVICE_COMMON_ERROR_NONE;
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
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	body = g_variant_new("(isi)", ret, pkgname, is_display);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "set_disp_option_notify");
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
}

/* set_noti_property */
void badge_set_setting_property(GVariant *parameters, GDBusMethodInvocation *invocation)
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
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));

	if (ret == BADGE_ERROR_NONE) {
		if (strcmp(property, "OPT_BADGE") == 0) {
			if (strcmp(value, "ON") == 0)
				is_display = 1;
			else
				is_display = 0;

			body = g_variant_new("(isi)", ret, pkgname, is_display);
			if (body == NULL) {
				ErrPrint("cannot make gvariant to noti");
				return;
			}
			ret = _send_notify(body, "set_disp_option_notify");
			if (ret != SERVICE_COMMON_ERROR_NONE) {
				ErrPrint("failed to send notify:%d\n", ret);
				return;
			}
		}
	} else {
		ErrPrint("failed to set noti property:%d\n", ret);
	}
}

/* get_noti_property */
void badge_get_setting_property(GVariant *parameters, GDBusMethodInvocation *invocation)
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
		ret = SERVICE_COMMON_ERROR_INVALID_PARAMETER;

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(si)", value, ret));
}

/*!
 * MAIN THREAD
 * Do not try to do anyother operation in these functions
 */
HAPI int badge_service_init(void)
{
	return SERVICE_COMMON_ERROR_NONE;
}

HAPI int badge_service_fini(void)
{
	return SERVICE_COMMON_ERROR_NONE;
}

/* End of a file */
