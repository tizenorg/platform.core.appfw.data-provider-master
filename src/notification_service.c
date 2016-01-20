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
#include "debug.h"

#include <notification_noti.h>
#include <notification_internal.h>
#include <notification_ipc.h>
#include <notification_setting_service.h>

#define NOTI_IPC_OBJECT_PATH "/org/tizen/noti_service"

#define PROVIDER_BUS_NAME "org.tizen.data_provider_service"
#define PROVIDER_OBJECT_PATH "/org/tizen/data_provider_service"
#define PROVIDER_NOTI_INTERFACE_NAME "org.tizen.data_provider_noti_service"
#define PROVIDER_BADGE_INTERFACE_NAME "org.tizen.data_provider_badge_service"

static GList *monitoring_app_list;
static void _update_noti(GDBusMethodInvocation *invocation, notification_h noti);

/*!
 * SERVICE HANDLER
 */
static int _send_notify(GVariant *body, char *cmd)
{
	GError *err = NULL;
	GList *target_list;
	char *target_bus_name = NULL;
	GDBusConnection *conn = service_common_get_connection();

	if(conn == NULL)
		return NOTIFICATION_ERROR_IO_ERROR;

	target_list = g_list_first(monitoring_app_list);
	for (; target_list != NULL; target_list = target_list->next) {
		target_bus_name = target_list->data;

		DbgPrint("emit signal to : %s", target_bus_name);
		if (g_dbus_connection_emit_signal(conn,
					target_bus_name,
					PROVIDER_OBJECT_PATH,
					PROVIDER_NOTI_INTERFACE_NAME,
					cmd,
					body,
					&err) == FALSE) {

			ErrPrint("g_dbus_connection_emit_signal() is failed");
			if (err != NULL) {
				ErrPrint("g_dbus_connection_emit_signal() err : %s",
						err->message);
				g_error_free(err);
			}
			return NOTIFICATION_ERROR_IO_ERROR;
		}
	}
	DbgPrint("_send_notify cmd %s done", cmd);
	return NOTIFICATION_ERROR_NONE;
}

/* add noti */
static void _add_noti(GDBusMethodInvocation *invocation, notification_h noti)
{
	int ret = 0;
	int priv_id = 0;
	GVariant *body = NULL;

	print_noti(noti);

	ret = notification_noti_insert(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ii)", ret, priv_id));

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to update a notification:%d\n", ret);
		return;
	}

	body = notification_ipc_make_gvariant_from_noti(noti);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "add_noti_notify");
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}

	DbgPrint("_insert_noti done !!");
}
void notification_add_noti(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = 0;
	notification_h noti = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		ret = notification_ipc_make_noti_from_gvariant(noti, parameters);
		if (ret == NOTIFICATION_ERROR_NONE) {
			ret = notification_noti_check_tag(noti);
			if (ret == NOTIFICATION_ERROR_NOT_EXIST_ID)
				_add_noti(invocation, noti);
			else if (ret == NOTIFICATION_ERROR_ALREADY_EXIST_ID)
				_update_noti(invocation, noti);
			else
				g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", ret));

			notification_free(noti);
		} else {
			g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", ret));
		}
	} else {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", NOTIFICATION_ERROR_OUT_OF_MEMORY));
	}

}


/* update noti */
static void _update_noti(GDBusMethodInvocation *invocation, notification_h noti)
{
	int ret = 0;
	GVariant *body = NULL;
	int priv_id = 0;

	print_noti(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	ret = notification_noti_update(noti);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to update a notification:%d", ret);
		if (ret == NOTIFICATION_ERROR_NOT_EXIST_ID) {
			ErrPrint("NOTIFICATION_ERROR_NOT_EXIST_ID !!");
		} else if (ret == NOTIFICATION_ERROR_FROM_DB) {
			ErrPrint("NOTIFICATION_ERROR_FROM_DB !!");
		}
	}
	g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ii)", ret, priv_id));

	body = notification_ipc_make_gvariant_from_noti(noti);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "update_noti_notify");

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
}

void notification_update_noti(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	notification_h noti = NULL;
	int ret = NOTIFICATION_ERROR_NONE;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		ret = notification_ipc_make_noti_from_gvariant(noti, parameters);
		if (ret == NOTIFICATION_ERROR_NONE) {
				_update_noti(invocation, noti);
				notification_free(noti);
		} else {
			g_dbus_method_invocation_return_value(
					invocation,
					g_variant_new("(ii)",
						ret,
						NOTIFICATION_PRIV_ID_NONE));
		}
	} else {
		g_dbus_method_invocation_return_value(
				invocation,
				g_variant_new("(ii)",
					NOTIFICATION_ERROR_OUT_OF_MEMORY,
					NOTIFICATION_PRIV_ID_NONE));
	}
}

/* load_noti_by_tag */
void notification_load_noti_by_tag(GVariant *parameters, GDBusMethodInvocation *invocation)
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
		g_dbus_method_invocation_return_value(
					invocation, body);
		notification_free(noti);
	}
	DbgPrint("_load_noti_by_tag done !!");
}

/* refresh_noti */
void notification_refresh_noti(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = 0;
	g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(i)", ret));
	ret = _send_notify(parameters, "refresh_noti_notify");
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
	DbgPrint("_refresh_noti_service done !!");
}

/* del_noti_single */
void notification_del_noti_single(GVariant *parameters, GDBusMethodInvocation *invocation)
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

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(ii)", ret, priv_id));

	if (ret != NOTIFICATION_ERROR_NONE || num_changes <= 0) {
		ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
		return;
	}

	body = g_variant_new("(ii)", 1, priv_id);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return;
	}
	ret = _send_notify(body, "delete_single_notify");

	g_variant_unref(body);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return;
	}
	DbgPrint("_del_noti_single done !!");
}

/* del_noti_multiple */
void notification_del_noti_multiple(GVariant *parameters, GDBusMethodInvocation *invocation)
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

	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(ii)", ret, num_deleted));

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete notifications:%d\n", ret);
		if (list_deleted != NULL)
			free(list_deleted);
		return;
	}

	if (num_deleted > 0) {
		GVariantBuilder * builder = g_variant_builder_new(G_VARIANT_TYPE("a(i)"));
		for (i = 0; i < num_deleted; i++) {
			g_variant_builder_add(builder, "(i)", *(list_deleted + i));
		}
		deleted_noti_list = g_variant_new("(a(i))", builder);
		ret = _send_notify(deleted_noti_list, "delete_multiple_notify");

		g_variant_builder_unref(builder);
		g_variant_unref(deleted_noti_list);
		free(list_deleted);

		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to send notify:%d\n", ret);
			return;
		}
	}
	DbgPrint("_del_noti_multiple done !!");
}

/* set_noti_property */
void notification_set_noti_property(GVariant *parameters, GDBusMethodInvocation *invocation)
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
	g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(i)", ret));
	if (pkgname)
		g_free(pkgname);
	if (property)
		g_free(property);
	if (value)
		g_free(value);

	DbgPrint("_set_noti_property_service done !! %d", ret);
}

/* get_noti_property */
void notification_get_noti_property(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &property);
	pkgname = string_get(pkgname);
	property = string_get(property);

	ret = notification_setting_db_get(pkgname, property, &value);
	g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(is)", ret, value));

	if (value != NULL)
		free(value);
	if (value)
		g_free(pkgname);
	if (property)
		g_free(property);

	DbgPrint("_get_noti_property_service done !! %d", ret);
}

/* update_noti_setting */
void notification_update_noti_setting(GVariant *parameters, GDBusMethodInvocation *invocation)
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

	g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(i)", ret));
	if (pkgname)
		g_free(pkgname);

	DbgPrint("_update_noti_setting_service done !! %d", ret);
}

/* update_noti_sys_setting */
void notification_update_noti_sys_setting(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = NOTIFICATION_ERROR_NONE;
	int do_not_disturb = 0;
	int visivility_class = 0;

	g_variant_get(parameters, "(ii)",
			&do_not_disturb,
			&visivility_class);

	DbgPrint("do_not_disturb [%d] visivility_class [%d]\n", do_not_disturb, visivility_class);
	ret = notification_setting_db_update_system_setting(do_not_disturb, visivility_class);

	g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(i)", ret));

	DbgPrint("_update_noti_sys_setting_service done !! %d", ret);
}

/* register service */
static int _monitoring_app_list_compare_cb(gconstpointer a, gconstpointer b)
{
	DbgPrint("compare %s : %s", (char *)a, (char *)b);
	if (strcmp(a, b) == 0)
		return 0;
	else
		return 1;


}
void notification_server_register(GVariant *parameters, GDBusMethodInvocation *invocation)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *bus_name = NULL;
	GList *added_list = NULL;

	g_variant_get(parameters, "(s)", &bus_name);
	if (bus_name != NULL) {
		added_list = g_list_find_custom(monitoring_app_list, bus_name,
		                                        (GCompareFunc)_monitoring_app_list_compare_cb);
	//	added_list = g_list_find(monitoring_app_list, bus_name);
		if (added_list == NULL) {
			monitoring_app_list = g_list_append(monitoring_app_list, strdup(bus_name));
			ErrPrint("_server_register_service : register success bus_name is %s , length : %d", bus_name, g_list_length(monitoring_app_list));
		} else {
			ErrPrint("_server_register_service : register bus_name %s already exist", bus_name);
		}

	} else {
		ErrPrint("_server_register_service : bus_name is NULL");
		ret = NOTIFICATION_ERROR_INVALID_PARAMETER;
	}
	if (bus_name)
		g_free(bus_name);
	g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(i)", ret));
}

/*
static void _handler_post_toast_message(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "i", ret);
	if (packet_reply) {
		if ((ret = service_common_unicast_packet(tcb, packet_reply)) < 0)
			ErrPrint("failed to send reply packet:%d\n", ret);
		packet_destroy(packet_reply);
	} else {
		ErrPrint("failed to create a reply packet\n");
	}

	if ((ret = service_common_multicast_packet(tcb, packet, TCB_CLIENT_TYPE_SERVICE)) < 0)
		ErrPrint("failed to send a multicast packet:%d\n", ret);
}
*/
/*
static void _handler_package_install(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *package_name = NULL;

	DbgPrint("_handler_package_install");
	if (packet_get(packet, "s", &package_name) == 1) {
		DbgPrint("package_name [%s]\n", package_name);
		// TODO : add codes to add a record to setting table

		if (ret != NOTIFICATION_ERROR_NONE)
			ErrPrint("failed to update setting[%d]\n", ret);
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}
*/

/*!
 * SERVICE PERMISSION CHECK
 */
/*
static void _permission_check_common(struct tcb *tcb, struct packet *packet)
{
	int ret_p = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "ii", NOTIFICATION_ERROR_PERMISSION_DENIED, 0);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0)
			ErrPrint("Failed to send a reply packet:%d", ret_p);
		packet_destroy(packet_reply);
	} else {
		ErrPrint("Failed to create a reply packet");
	}
}

static void _permission_check_refresh(struct tcb *tcb, struct packet *packet)
{
	int ret_p = 0;
	struct packet *packet_reply = NULL;
g_dbus_connection_unregister_object(__gdbus_conn, local_port_id);
	packet_reply = packet_create_reply(packet, "i", NOTIFICATION_ERROR_PERMISSION_DENIED);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0)
			ErrPrint("Failed to send a reply packet:%d", ret_p);
		packet_destroy(packet_reply);
	} else {
		ErrPrint("Failed to create a reply packet");
	}
}

static void _permission_check_property_get(struct tcb *tcb, struct packet *packet)
{
	int ret_p = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "is", NOTIFICATION_ERROR_PERMISSION_DENIED, NULL);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0)
			ErrPrint("Failed to send a reply packet:%d", ret_p);
		packet_destroy(packet_reply);
	} else {
		ErrPrint("Failed to create a reply packet");
	}
}*/

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

	notification_get_list(NOTIFICATION_TYPE_NONE, -1, &noti_list);
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

/*!
 * Managing setting DB
 */
/*
static int _invoke_package_change_event(enum pkgmgr_event_type event_type, const char *pkgname)
{
	int ret = 0;
	struct packet *packet = NULL;
	struct service_context *svc_ctx = notification_service_info->svc_ctx;

	DbgPrint("pkgname[%s], event_type[%d]\n", pkgname, event_type);

	if (event_type == PKGMGR_EVENT_INSTALL) {
		packet = packet_create_noack("package_install", "s", pkgname);
	} else if (event_type == PKGMGR_EVENT_UNINSTALL) {
		packet = packet_create_noack("package_uninstall", "s", pkgname);
	} else {
		goto out;
	}

	if (packet == NULL) {
		ErrPrint("packet_create_noack failed\n");
		ret = -1;
		goto out;
	}

	if ((ret = service_common_send_packet_to_service(svc_ctx, NULL, packet)) != 0) {
		ErrPrint("service_common_send_packet_to_service failed[%d]\n", ret);
		ret = -1;
		goto out;
	}

out:
	if (ret != 0 && packet)
		packet_destroy(packet);

	DbgPrint("_invoke_package_change_event returns [%d]\n", ret);
	return ret;
}
*/
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
