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

extern int notification_service_init(void);
extern int notification_service_fini(void);

int notification_add_noti(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_update_noti(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_refresh_noti(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_del_noti_single(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_del_noti_multiple(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_set_noti_property(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_get_noti_property(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_get_noti_count(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_update_noti_setting(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_update_noti_sys_setting(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_load_noti_by_tag(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_load_noti_by_priv_id(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_load_grouping_list(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_load_detail_list(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_get_setting_array(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_get_setting_by_package_name(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_load_system_setting(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_add_noti_template(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_get_noti_template(GVariant *parameters, GVariant **reply_body, pid_t pid, uid_t uid);
int notification_get_noti_package_template(GVariant *parameters, GVariant **reply_body, uid_t uid);
int notification_register_dbus_interface();

/* End of a file */
