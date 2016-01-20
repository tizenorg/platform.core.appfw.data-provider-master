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

/* add_shortcut */
int shortcut_add(GVariant *parameters, GVariant **reply_body)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	ret = send_notify(parameters, "add_shortcut_notify", SHORTCUT_SERVICE);
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("Cannot make reply body");
		return SHORTCUT_ERROR_OUT_OF_MEMORY;
	}

	return ret;
}

/* add_shortcut_widget */
int shortcut_add_widget(GVariant *parameters, GVariant **reply_body)
{
	int ret = SERVICE_COMMON_ERROR_NONE;

	ret = send_notify(parameters, "add_shortcut_widget_notify", SHORTCUT_SERVICE);
	if (ret != SERVICE_COMMON_ERROR_NONE) {
		ErrPrint("failed to send notify:%d\n", ret);
		return ret;
	}

	*reply_body = g_variant_new("(i)", ret);
	if (*reply_body == NULL) {
		ErrPrint("Cannot make reply body");
		return SHORTCUT_ERROR_OUT_OF_MEMORY;
	}

	return ret;
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
