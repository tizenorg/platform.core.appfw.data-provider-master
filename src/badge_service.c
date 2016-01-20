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

/* insert_badge */
int badge_insert(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
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
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to insert badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(is)", ret, pkgname);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	ret = send_notify(body, "insert_badge_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

/* delete_badge */
int badge_delete(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
	char *pkgname = NULL;
	char *caller = NULL;
	GVariant *body = NULL;

	g_variant_get(parameters, "(ss)", &pkgname, &caller);
	pkgname = string_get(pkgname);
	caller = string_get(caller);

	if (pkgname != NULL && caller != NULL) {
		ret = badge_db_delete(pkgname, pkgname);
	} else {
		return BADGE_ERROR_INVALID_PARAMETER;
	}

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to delete badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(is)", ret, pkgname);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "delete_badge_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_badge_count */
int badge_set_badge_count(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
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
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set badge :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(isi)", ret, pkgname, count);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "set_badge_count_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_disp_option */
int badge_set_display_option(GVariant *parameters, GVariant **reply_body)
{
	int ret = BADGE_ERROR_NONE;
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
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to set display option :%d\n", ret);
		return ret;
	}

	body = g_variant_new("(isi)", ret, pkgname, is_display);
	if (body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	ret = send_notify(body, "set_disp_option_notify", BADGE_SERVICE);
	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* set_noti_property */
int badge_set_setting_property(GVariant *parameters, GVariant **reply_body)
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

			body = g_variant_new("(isi)", ret, pkgname, is_display);
			if (body == NULL) {
				ErrPrint("cannot make gvariant to noti");
				return BADGE_ERROR_OUT_OF_MEMORY;
			}
			ret = send_notify(body, "set_disp_option_notify", BADGE_SERVICE);
			if (ret != BADGE_ERROR_NONE) {
				ErrPrint("failed to send notify:%d\n", ret);
				return ret;
			}
		}
	} else {
		ErrPrint("failed to set noti property:%d\n", ret);
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("cannot make gvariant to noti");
		return BADGE_ERROR_OUT_OF_MEMORY;
	}
	return ret;
}

/* get_noti_property */
int badge_get_setting_property(GVariant *parameters, GVariant **reply_body)
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
		return BADGE_ERROR_INVALID_PARAMETER;

	if (ret != BADGE_ERROR_NONE) {
		ErrPrint("failed to setting db get :%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(si)", value, ret);
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
	return BADGE_ERROR_NONE;
}

HAPI int badge_service_fini(void)
{
	return BADGE_ERROR_NONE;
}

/* End of a file */
