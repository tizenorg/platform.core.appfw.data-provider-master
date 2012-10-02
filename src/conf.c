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

#include <Ecore_X.h>

#include "conf.h"

struct conf g_conf = {
	.width = 0,
	.height = 0,

	.base_width = 720,
	.base_height = 1280,

	.minimum_period = 1.0f,

	.default_conf.script = "edje",
	.default_conf.abi = "c",
	.default_conf.pd_group = "disclosure",
	.default_conf.period = -1.0f,

	.launch_key.name = "name",
	.launch_key.secured = "secured",
	.launch_key.abi = "abi",

	.default_packet_time = 0.0001f,

	.empty_content = "",
	.empty_title = "",

	.default_content = "default",
	.default_title = "",

	.minimum_space = 5242880,

	.replace_tag = "/APPID/",

	.slave_ttl = 30.0f,

	.max_log_line = 1000,
	.max_log_file = 3,

	.sqlite_flush_max = 1048576,

	.path = {
		.conf = "/opt/live/%s/etc/%s.conf",
		.image = "/opt/share/live_magazine/",
		.slave_log = "/opt/share/live_magazine/log",
		.script = "/opt/live/%s/res/script/%s.edj",
		.root = "/opt/live/",
		.script_port = "/opt/live/script_port/",
		.db = "/opt/dbspace/.livebox.db",
	},

	.size = {
		{
			.width = 172,
			.height = 172,	/* Slot type 1 */
		},
		{
			.width = 348,
			.height = 172,	/* Slot type 2 */
		},
		{
			.width = 348,
			.height = 348,	/* Slot type 3 */
		},
		{
			.width = 700,
			.height = 348,  /* Slot type 4 */
		},
		{
			.width = 700,
			.height = 172, /* Slot type 5 4x1 */
		},
		{
			.width = 700,
			.height = 700, /* Slot type 6 4x4 */
		},
	},

	.max_size_type = MAX_SIZE_LIST,
	.quality = "quality=100 compress=1",
	.error = "/opt/apps/com.samsung."PACKAGE"/res/images/error.png",
	.ping_time = 240.0f,
	.slave_max_load = 10,
	.vconf_sys_cluster = "file/private/com.samsung.cluster-home/system_cluster",
	.max_pended_ctx_events = 256,
};

void conf_update_size(void)
{
	ecore_x_window_size_get(0, &g_conf.width, &g_conf.height);
}

/* End of a file */
