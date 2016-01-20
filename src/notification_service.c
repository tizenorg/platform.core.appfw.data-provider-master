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

static int _update_noti(GVariant **reply_body, notification_h noti);

/*!
 * SERVICE HANDLER
 */

/* add noti */
static int _add_noti(GVariant **reply_body, notification_h noti)
{
	int ret = 0;
	int priv_id = 0;
	GVariant *body = NULL;

	print_noti(noti);
	ret = notification_noti_insert(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to update a notification:%d\n", ret);
		return ret;
	}

	body = notification_ipc_make_gvariant_from_noti(noti);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "add_noti_notify", NOTIFICATION_SERVICE);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(ii)", ret, priv_id);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_insert_noti done !!");
	return ret;
}

int notification_add_noti(GVariant *parameters, GVariant **reply_body)
{
	int ret = 0;
	notification_h noti = NULL;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		if (ret == NOTIFICATION_ERROR_NONE) {
			ret = notification_noti_check_tag(noti);
			if (ret == NOTIFICATION_ERROR_NOT_EXIST_ID)
				ret = _add_noti(reply_body, noti);
			else if (ret == NOTIFICATION_ERROR_ALREADY_EXIST_ID)
				ret = _update_noti(reply_body, noti);

			notification_free(noti);
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}


	DbgPrint("notification_add_noti ret : %d", ret);
	return ret;
}


/* update noti */
static int _update_noti(GVariant **reply_body, notification_h noti)
{
	int ret = 0;
	GVariant *body = NULL;
	int priv_id = 0;

	print_noti(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]", priv_id);

	ret = notification_noti_update(noti);
	if (ret != NOTIFICATION_ERROR_NONE)
		return ret;

	body = notification_ipc_make_gvariant_from_noti(noti);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_IO_ERROR;
	}

	ret = send_notify(body, "update_noti_notify", NOTIFICATION_SERVICE);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(ii)", ret, priv_id);
	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_update_noti done !!");
	return ret;
}

int notification_update_noti(GVariant *parameters, GVariant **reply_body)
{
	notification_h noti = NULL;
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(v)", &body);
		ret = notification_ipc_make_noti_from_gvariant(noti, body);
		if (ret == NOTIFICATION_ERROR_NONE) {
				ret = _update_noti(reply_body, noti);
				notification_free(noti);
		}
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* load_noti_by_tag */
int notification_load_noti_by_tag(GVariant *parameters, GVariant **reply_body)
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
		*reply_body = g_variant_new("(iv)", ret, body);

		notification_free(noti);
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("_load_noti_by_tag done !!");
	return ret;
}

/* load_noti_by_priv_id */
int notification_load_noti_by_priv_id(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	int priv_id;
	char *pkgname;
	notification_h noti = NULL;
	GVariant *body = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(si)", &pkgname, &priv_id);
		DbgPrint("load_noti_by_priv_id pkgname : %s, priv_id : %d ", pkgname, priv_id);
		ret = notification_noti_get_by_priv_id(noti, pkgname, priv_id);

		DbgPrint("notification_noti_get_by_priv_id ret : %d", ret);
		print_noti(noti);

		body = notification_ipc_make_gvariant_from_noti(noti);
		*reply_body = g_variant_new("(iv)", ret, body);

		notification_free(noti);
	} else {
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_load_noti_by_priv_id done !!");
	return ret;
}

/* load_noti_grouping_list */
int notification_load_grouping_list(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	notification_h noti = NULL;
	GVariant *body = NULL;
	notification_type_e type;
	notification_list_h get_list = NULL;
	notification_list_h list_iter = NULL;
	GVariantBuilder *builder = NULL;
	int count;
	int noti_cnt = 0;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(ii)", &type, &count);
		DbgPrint("load grouping list type : %d, count : %d ", type, count);

		ret = notification_noti_get_grouping_list(type, count, &get_list);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));
		if (get_list) {

			list_iter = notification_list_get_head(get_list);
			do {
				noti = notification_list_get_data(list_iter);
				body = notification_ipc_make_gvariant_from_noti(noti);
				g_variant_builder_add(builder, "(v)", body);
				list_iter = notification_list_get_next(list_iter);
				noti_cnt++;
			} while (list_iter != NULL);

		}
		*reply_body = g_variant_new("(ia(v))", ret, builder);
		notification_free_list(get_list);

	} else {
		ErrPrint("cannot create notification");
		ret = NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("load grouping list done !!");
	return ret;
}

/* get_setting_array */
int notification_get_setting_array(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;
	GVariantBuilder *builder = NULL;
	int count;
	int i = 0;
	notification_setting_h setting_array = NULL;
	notification_setting_h temp = NULL;

	noti_setting_get_setting_array(&setting_array, &count);
	DbgPrint("get setting array : %d", count);
	builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));

	if (setting_array) {
		for (i = 0; i < count; i++) {
			temp = setting_array + i;
			body = notification_ipc_make_gvariant_from_setting(temp);
			g_variant_builder_add(builder, "(v)", body);
		}
	}
	*reply_body = g_variant_new("(iia(v))", ret, count, builder);
	return ret;
}

/* get_setting_array */
int notification_get_setting_by_package_name(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;
	char *pkgname = NULL;
	notification_setting_h setting = NULL;

	g_variant_get(parameters, "(s)", &pkgname);
	DbgPrint("get setting by pkgname : %s", pkgname);

	ret = noti_setting_service_get_setting_by_package_name(pkgname, &setting);
	if (ret == NOTIFICATION_ERROR_NONE)
		body = notification_ipc_make_gvariant_from_setting(setting);

	if (body != NULL) {
		DbgPrint("get setting by pkgname ret @@~~ : %d", ret);
		*reply_body = g_variant_new("(iv)", ret, body);
	}

	return ret;
}

/* load_system_setting */
int notification_load_system_setting(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	GVariant *body = NULL;
	notification_system_setting_h setting = NULL;

	ret = noti_system_setting_load_system_setting(&setting);
	if (ret == NOTIFICATION_ERROR_NONE)
		body = notification_ipc_make_gvariant_from_system_setting(setting);

	if (body != NULL)
		*reply_body = g_variant_new("(iv)", ret, body);

	return ret;
}

/* load_noti_detail_list */
int notification_load_detail_list(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	notification_h noti = NULL;
	GVariant *body = NULL;
	notification_list_h get_list = NULL;
	notification_list_h list_iter = NULL;
	GVariantBuilder *builder = NULL;
	char *pkgname = NULL;
	int group_id;
	int priv_id;
	int count;
	int noti_cnt = 0;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		g_variant_get(parameters, "(siii)", &pkgname, &group_id, &priv_id, &count);
		DbgPrint("load detail list pkgname : %s, group_id : %d, priv_id : %d, count : %d ",
				pkgname, group_id, priv_id, count);

		ret = notification_noti_get_detail_list(pkgname, group_id, priv_id,
				count, &get_list);
		if (ret != NOTIFICATION_ERROR_NONE)
			return ret;

		builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));
		if (get_list) {

			list_iter = notification_list_get_head(get_list);
			do {
				noti = notification_list_get_data(list_iter);
				body = notification_ipc_make_gvariant_from_noti(noti);
				g_variant_builder_add(builder, "(v)", body);
				list_iter = notification_list_get_next(list_iter);
				noti_cnt++;
			} while (list_iter != NULL);

		}
		*reply_body = g_variant_new("(ia(v))", ret, builder);
		notification_free_list(get_list);

	} else {
		ErrPrint("cannot create notification");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("load detail list done !!");
	return ret;
}

/* refresh_noti */
int notification_refresh_noti(GVariant *parameters, GVariant **reply_body)
{
	int ret = 0;
	ret = send_notify(parameters, "refresh_noti_notify", NOTIFICATION_SERVICE);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL)  {
		ErrPrint("cannot create reply");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("_refresh_noti_service done !!");
	return ret;
}

/* del_noti_single */
int notification_del_noti_single(GVariant *parameters, GVariant **reply_body)
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
	if (ret != NOTIFICATION_ERROR_NONE || num_changes <= 0) {
		ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
		return ret;
	}

	body = g_variant_new("(ii)", 1, priv_id);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return NOTIFICATION_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "delete_single_notify", NOTIFICATION_SERVICE);
	g_variant_unref(body);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(ii)", ret, priv_id);
	DbgPrint("_del_noti_single done !!");
	return ret;
}

/* del_noti_multiple */
int notification_del_noti_multiple(GVariant *parameters, GVariant **reply_body)
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

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to delete notifications:%d\n", ret);
		if (list_deleted != NULL)
			free(list_deleted);
		return ret;
	}

	if (num_deleted > 0) {
		GVariantBuilder * builder = g_variant_builder_new(G_VARIANT_TYPE("a(i)"));
		for (i = 0; i < num_deleted; i++) {
			g_variant_builder_add(builder, "(i)", *(list_deleted + i));
		}
		deleted_noti_list = g_variant_new("(a(i))", builder);
		ret = send_notify(deleted_noti_list, "delete_multiple_notify", NOTIFICATION_SERVICE);

		g_variant_builder_unref(builder);
		g_variant_unref(deleted_noti_list);
		free(list_deleted);

		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to send notify:%d\n", ret);
			return ret;
		}
	}

	*reply_body = g_variant_new("(ii)", ret, num_deleted);
	DbgPrint("_del_noti_multiple done !!");
	return ret;
}

/* set_noti_property */
int notification_set_noti_property(GVariant *parameters, GVariant **reply_body)
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
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db set : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);

	DbgPrint("_set_noti_property_service done !! %d", ret);
	return ret;
}

/* get_noti_property */
int notification_get_noti_property(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &property);
	pkgname = string_get(pkgname);
	property = string_get(property);

	ret = notification_setting_db_get(pkgname, property, &value);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db get : %d\n", ret);
		return ret;
	}
	*reply_body = g_variant_new("(is)", ret, value);

	if (value != NULL)
		free(value);

	DbgPrint("_get_noti_property_service done !! %d", ret);
	return ret;
}

/* get_noti_count */
int notification_get_noti_count(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	notification_type_e type;
	char *pkgname;
	int group_id;
	int priv_id;
	int noti_count;

	g_variant_get(parameters, "(isii)", &type, &pkgname, &group_id, &priv_id);
	pkgname = string_get(pkgname);

	ret = notification_noti_get_count(type, pkgname, group_id, priv_id,
			&noti_count);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to get count : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(ii)", ret, noti_count);
	DbgPrint("_get_noti_property_service done !! %d", ret);
	return ret;
}

/* update_noti_setting */
int notification_update_noti_setting(GVariant *parameters, GVariant **reply_body)
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
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db update : %d\n", ret);
		return ret;
	}

	if (pkgname)
		g_free(pkgname);

	*reply_body = g_variant_new("(i)", ret);
	DbgPrint("_update_noti_setting_service done !! %d", ret);
	return ret;
}

/* update_noti_sys_setting */
int notification_update_noti_sys_setting(GVariant *parameters, GVariant **reply_body)
{
	int ret = NOTIFICATION_ERROR_NONE;
	int do_not_disturb = 0;
	int visivility_class = 0;

	g_variant_get(parameters, "(ii)",
			&do_not_disturb,
			&visivility_class);

	DbgPrint("do_not_disturb [%d] visivility_class [%d]\n", do_not_disturb, visivility_class);
	ret = notification_setting_db_update_system_setting(do_not_disturb, visivility_class);
	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to setting db update system setting : %d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	DbgPrint("_update_noti_sys_setting_service done !! %d", ret);
	return ret;
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

	notification_noti_get_grouping_list(NOTIFICATION_TYPE_NONE, -1, &noti_list);
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
