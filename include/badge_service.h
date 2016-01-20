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

#include <gio/gio.h>

extern int badge_service_init(void);
extern int badge_service_fini(void);

void badge_server_register(GVariant *parameters, GDBusMethodInvocation *invocation);
void badge_insert(GVariant *parameters, GDBusMethodInvocation *invocation);
void badge_delete(GVariant *parameters, GDBusMethodInvocation *invocation);
void badge_set_badge_count(GVariant *parameters, GDBusMethodInvocation *invocation);
void badge_set_display_option(GVariant *parameters, GDBusMethodInvocation *invocation);
void badge_set_setting_property(GVariant *parameters, GDBusMethodInvocation *invocation);
void badge_get_setting_property(GVariant *parameters, GDBusMethodInvocation *invocation);

/* End of a file */
