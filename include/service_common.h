/*
 * Copyright 2016  Samsung Electronics Co., Ltd
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

#include <gio/gio.h>
#include <notification.h>
#include <badge.h>
#include <shortcut.h>
#include <stdlib.h>

typedef enum {
	NOTIFICATION_SERVICE = 0,
	SHORTCUT_SERVICE,
	BADGE_SERVICE,
} service_type;

enum service_common_error {
	SERVICE_COMMON_ERROR_NONE = 0,
	SERVICE_COMMON_ERROR_INVALID_PARAMETER = -1,
	SERVICE_COMMON_ERROR_ALREADY_STARTED = -2,
	SERVICE_COMMON_ERROR_FAULT = -3,
	SERVICE_COMMON_ERROR_PERMISSION_DENIED = -4,
	SERVICE_COMMON_ERROR_IO_ERROR = -5,
	SERVICE_COMMON_ERROR_OUT_OF_MEMORY = -6,
	SERVICE_COMMON_ERROR_NOT_EXIST = -7,
	SERVICE_COMMON_ERROR_ALREADY_EXIST = -8,
};

typedef struct monitoring_info {
	int watcher_id;
	char *bus_name;
} monitoring_info_s;

void print_noti(notification_h noti);
int send_notify(GVariant *body, char *cmd, GList *monitoring_app_list, char *interface_name);
int service_register(GVariant *parameters, GVariant **reply_body, const gchar *sender,
	GBusNameAppearedCallback name_appeared_handler,
	GBusNameVanishedCallback name_vanished_handler,
	GList **monitoring_list);
GDBusConnection *service_common_get_connection();
int service_common_register_dbus_interface(char *introspection_xml, GDBusInterfaceVTable interface_vtable);

/* End of a file */
