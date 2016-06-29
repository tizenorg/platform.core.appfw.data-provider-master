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

#include <gio/gio.h>
#include <sys/types.h>

extern int badge_service_init(void);
extern int badge_service_fini(void);

int badge_get_badge_existing(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_get_badge_list(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_insert(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_delete(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_set_badge_count(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_get_badge_count(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_set_display_option(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_get_display_option(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_set_setting_property(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_get_setting_property(GVariant *parameters, GVariant **reply_body, uid_t uid);
int badge_register_dbus_interface();

/* End of a file */
