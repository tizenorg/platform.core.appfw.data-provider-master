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

#include <dlog.h>
#include <gio/gio.h>
#include <sys/smack.h>
#include <badge.h>
#include <badge_db.h>
#include <badge_setting_service.h>
#include <badge_internal.h>
#include <tzplatform_config.h>

#include "service_common.h"
#include "badge_service.h"
#include "debug.h"

#define PROVIDER_BADGE_INTERFACE_NAME "org.tizen.data_provider_badge_service"
static GHashTable *_monitoring_hash = NULL;

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
		DbgPrint("name vanished uid : %d", info->uid);
		g_bus_unwatch_name(info->watcher_id);
	}
}

static void _badge_dbus_method_call_handler(GDBusConnection *conn,
		const gchar *sender, const gchar *object_path,
		const gchar *iface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	DbgPrint("badge method_name: %s", method_name);

	GVariant *reply_body = NULL;
	int ret = BADGE_ERROR_INVALID_PARAMETER;
	uid_t uid = get_sender_uid(sender);

	if (g_strcmp0(method_name, "badge_service_register") == 0)
		ret = service_register(parameters, &reply_body, sender,
				_on_name_appeared, _on_name_vanished, &_monitoring_hash, uid);
	else if (g_strcmp0(method_name, "get_badge_existing") == 0)
		ret = badge_get_badge_existing(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_list") == 0)
		ret = badge_get_badge_list(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "insert_badge") == 0)
		ret = badge_insert(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "delete_badge") == 0)
		ret = badge_delete(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "set_badge_count") == 0)
		ret = badge_set_badge_count(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_badge_count") == 0)
		ret = badge_get_badge_count(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "set_disp_option") == 0)
		ret = badge_set_display_option(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_disp_option") == 0)
		ret = badge_get_display_option(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "set_noti_property") == 0)
		ret = badge_set_setting_property(parameters, &reply_body, uid);
	else if (g_strcmp0(method_name, "get_noti_property") == 0)
		ret = badge_get_setting_property(parameters, &reply_body, uid);

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

int badge_register_dbus_interface()
{
	static gchar introspection_xml[] =
			"  <node>"
			"  <interface name='org.tizen.data_provider_badge_service'>"
			"        <method name='badge_service_register'>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"
			"        <method name='get_badge_existing'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='exist' direction='out'/>"
			"        </method>"
			"        <method name='get_list'>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='a(v)' name='badge_list' direction='out'/>"
			"        </method>"
			"        <method name='insert_badge'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='writable_pkg' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='delete_badge'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='set_badge_count'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='count' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='get_badge_count'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='count' direction='out'/>"
			"        </method>"

			"        <method name='set_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='caller' direction='in'/>"
			"          <arg type='i' name='is_display' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='get_disp_option'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='i' name='is_display' direction='out'/>"
			"        </method>"

			"        <method name='set_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='s' name='value' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"        </method>"

			"        <method name='get_noti_property'>"
			"          <arg type='s' name='pkgname' direction='in'/>"
			"          <arg type='s' name='property' direction='in'/>"
			"          <arg type='i' name='uid' direction='in'/>"
			"          <arg type='s' name='value' direction='out'/>"
			"        </method>"
			"  </interface>"
			"  </node>";

	return service_common_register_dbus_interface(introspection_xml, _badge_interface_vtable);
}

static int _validate_and_set_param_uid_with_uid(uid_t uid, uid_t *param_uid)
{
	int ret = BADGE_ERROR_NONE;

	if (uid > NORMAL_UID_BASE && uid != *param_uid) {
		ErrPrint("invalid sender uid : %d, param_uid : %d", uid, *param_uid);
		return BADGE_ERROR_INVALID_PARAMETER;
	} else if (uid <= NORMAL_UID_BASE) {
		if (*param_uid <= NORMAL_UID_BASE) {
			if (*param_uid != uid)
				return BADGE_ERROR_INVALID_PARAMETER;
			*param_uid = tzplatform_getuid(TZ_SYS_DEFAULT_USER);
		}
	}
	return ret;
}

static void _release_badge_info(gpointer data)
{
	badge_info_s *badge = (badge_info_s *)data;
	if (badge == NULL)
		return;
	if (badge->pkg)
		free(badge->pkg);
	free(badge);
}

/* get_badge_existing */
int badge_get_badge_existing(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	bool existing = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(&si)", &pkgname, &param_uid);
	DbgPrint("badge_get_badge_existing %s", pkgname);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL)
		ret = badge_db_is_existing(pkgname, &existing, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to get badge existing :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", existing);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("badge_get_badge_existing service done");
	return ret;
}

/* get_list */
int badge_get_badge_list(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	GList *badge_list = NULL;
	GList *iter_list = NULL;
	GVariant *body = NULL;
	badge_info_s *badge;
	GVariantBuilder *builder;
	int ret;
	uid_t param_uid;

	g_variant_get(parameters, "(i)", &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	ret = badge_db_get_list(&badge_list, param_uid);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("badge get list fail : %d", ret);
		return ret;
	}

	builder = g_variant_builder_new(G_VARIANT_TYPE("a(v)"));
	iter_list = g_list_first(badge_list);
	for (; iter_list != NULL; iter_list = iter_list->next) {
		badge = iter_list->data;
		body = g_variant_new("(&si)", badge->pkg, badge->badge_count);
		g_variant_builder_add(builder, "(v)", body);
	}
	g_list_free_full(badge_list, (GDestroyNotify)_release_badge_info);

	*reply_body = g_variant_new("(a(v))", builder);
	g_variant_builder_unref(builder);

	if (*reply_body == NULL) {
		ErrPrint("cannot make reply_body");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	DbgPrint("badge_get_badge_list done !!");
	return BADGE_ERROR_NONE;
}

/* insert_badge */
int badge_insert(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *writable_pkg = NULL;
	char *caller = NULL;
	GVariant *body = NULL;
	GList *monitoring_list = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&s&s&si)", &pkgname, &writable_pkg, &caller, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL && writable_pkg != NULL && caller != NULL)
		ret = badge_db_insert(pkgname, writable_pkg, caller, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to insert badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(si)", pkgname, param_uid);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, &param_uid);
	ret = send_notify(body, "insert_badge_notify", monitoring_list, PROVIDER_BADGE_INTERFACE_NAME);
	g_variant_unref(body);

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

/* delete_badge */
int badge_delete(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	GVariant *body = NULL;
	GList *monitoring_list = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&s&si)", &pkgname, &caller, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL && caller != NULL) {
		ret = badge_db_delete(pkgname, caller, param_uid);
	} else {
		return BADGE_ERROR_INVALID_PARAMETER;
	}

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to delete badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(si)", pkgname, param_uid);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, &param_uid);
	ret = send_notify(body, "delete_badge_notify", monitoring_list, PROVIDER_BADGE_INTERFACE_NAME);
	g_variant_unref(body);

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_badge_count */
int badge_set_badge_count(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	unsigned int count = 0;
	GVariant *body = NULL;
	GList *monitoring_list = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&s&sii)", &pkgname, &caller, &count, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL && caller != NULL)
		ret = badge_db_set_count(pkgname, caller, count, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(sii)", pkgname, count, param_uid);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, &param_uid);
	ret = send_notify(body, "set_badge_count_notify", monitoring_list, PROVIDER_BADGE_INTERFACE_NAME);
	g_variant_unref(body);

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}
	DbgPrint("send badge count notify done ret : %d", ret);

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_badge_count */
int badge_get_badge_count(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	unsigned int count = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(&si)", &pkgname, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL)
		ret =  badge_db_get_count(pkgname, &count, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to get badge count :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", count);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("badge_get_badge_count service done");
	return ret;
}

/* set_disp_option */
int badge_set_display_option(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	unsigned int is_display = 0;
	GVariant *body = NULL;
	GList *monitoring_list = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&s&sii)", &pkgname, &caller, &is_display, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	DbgPrint("set disp option : %s, %s, %d", pkgname, caller, is_display);

	if (pkgname != NULL && caller != NULL)
		ret = badge_db_set_display_option(pkgname, is_display, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set display option :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(sii)", pkgname, is_display, param_uid);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, &param_uid);
	ret = send_notify(body, "set_disp_option_notify", monitoring_list, PROVIDER_BADGE_INTERFACE_NAME);
	g_variant_unref(body);

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	DbgPrint("set disp option done %d", ret);
	return ret;
}

/* get_disp_option */
int badge_get_display_option(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	unsigned int is_display = 0;
	uid_t param_uid;

	g_variant_get(parameters, "(&si)", &pkgname, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	DbgPrint("get disp option : %s", pkgname);

	if (pkgname != NULL)
		ret = badge_db_get_display_option(pkgname, &is_display, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to get display option :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", is_display);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_noti_property */
int badge_set_setting_property(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = 0;
	int is_display = 0;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;
	GVariant *body = NULL;
	GList *monitoring_list = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&s&s&si)", &pkgname, &property, &value, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL && property != NULL && value != NULL)
		ret = badge_setting_db_set(pkgname, property, value, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to setting db set :%d\n", ret);
		return ret;
	}

	if (ret == BADGE_ERROR_NONE) {
		if (strcmp(property, "OPT_BADGE") == 0) {
			if (strcmp(value, "ON") == 0)
				is_display = 1;
			else
				is_display = 0;

			body = g_variant_new("(si)", pkgname, is_display);
			if (body == NULL) {
				ErrPrint("cannot make gvariant to noti");
				return BADGE_ERROR_OUT_OF_MEMORY;
			}

			monitoring_list = (GList *)g_hash_table_lookup(_monitoring_hash, &param_uid);
			ret = send_notify(body, "set_disp_option_notify", monitoring_list, PROVIDER_BADGE_INTERFACE_NAME);
			g_variant_unref(body);
			if (ret != BADGE_ERROR_NONE) {
				ErrPrint("failed to send notify:%d\n", ret);
				return ret;
			}
		}
	} else {
		ErrPrint("failed to set noti property:%d\n", ret);
	}

	*reply_body = g_variant_new("()");
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_noti_property */
int badge_get_setting_property(GVariant *parameters, GVariant **reply_body, uid_t uid)
{
	int ret = 0;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;
	uid_t param_uid;

	g_variant_get(parameters, "(&s&si)", &pkgname, &property, &param_uid);

	ret = _validate_and_set_param_uid_with_uid(uid, &param_uid);
	if (ret != BADGE_ERROR_NONE)
		return ret;

	if (pkgname != NULL && property != NULL)
		ret = badge_setting_db_get(pkgname, property, &value, param_uid);
	else
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to setting db get :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(s)", value);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/*!
 * MAIN THREAD
 * Do not try to do anyother operation in these functions
 */
HAPI int badge_service_init(void)
{
	int result;

	_monitoring_hash = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, free_monitoring_list);
	result = badge_db_init();
	if (result != BADGE_ERROR_NONE) {
		ErrPrint("badge db init fail %d", result);
		return result;
	}

	result = badge_register_dbus_interface();
	if (result != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("badge register dbus fail %d", result);
		return BADGE_ERROR_IO_ERROR;
	}

	return BADGE_ERROR_NONE;
}

HAPI int badge_service_fini(void)
{
	return BADGE_ERROR_NONE;
}

/* End of a file */
