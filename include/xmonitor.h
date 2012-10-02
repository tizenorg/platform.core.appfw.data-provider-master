/*
 * com.samsung.live-data-provider
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: Sung-jae Park <nicesj.park@samsung.com>, Youngjoo Park <yjoo93.park@samsung.com>
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
 *
 */

enum xmonitor_event {
	XMONITOR_PAUSED,
	XMONITOR_RESUMED,

	XMONITOR_ERROR = 0xFFFFFFFF, /* To specify the size of this enum */
};

extern int xmonitor_init(void);
extern void xmonitor_fini(void);
extern int xmonitor_update_state(int pid);
extern int xmonitor_add_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data);
extern int xmonitor_del_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data);
extern int xmonitor_is_paused(void);
extern void xmonitor_handle_state_changes(void);

/* End of a file */
