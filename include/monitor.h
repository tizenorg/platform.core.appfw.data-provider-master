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

enum monitor_event_type {
	MONITOR_EVENT_UNKNOWN = 0x0,
	MONITOR_EVENT_CREATED = CMD_MONITOR_CREATE,
	MONITOR_EVENT_DESTROYED = CMD_MONITOR_DESTROY,
	MONITOR_EVENT_PAUSED = CMD_MONITOR_PAUSE,
	MONITOR_EVENT_RESUMED = CMD_MONITOR_RESUME
};

struct monitor_client;

extern struct monitor_client *monitor_create_client(const char *widget_id, pid_t pid, int handle);
extern int monitor_destroy_client(struct monitor_client *monitor);

extern struct monitor_client *monitor_find_client_by_pid(const char *widget_id, pid_t pid);
extern struct monitor_client *monitor_find_client_by_handle(const char *widget_id, int handle);

extern int monitor_multicast_state_change_event(const char *widget_id, enum monitor_event_type event, const char *instance_id, const char *content_info);

/* End of a file */
