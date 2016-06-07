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

extern int shortcut_service_init(void);
extern int shortcut_service_fini(void);

int shortcut_add(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid);
int shortcut_add_widget(GVariant *parameters, GVariant **reply_body, GList *monitoring_list, uid_t uid);
int shortcut_register_dbus_interface();
int shortcut_get_shortcut_service_list(GVariant *parameters, GVariant **reply_body, uid_t uid);
int shortcut_check_privilege(void);
/* End of a file */
