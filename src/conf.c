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

	.log = {
		.error = "/opt/apps/com.samsung."PACKAGE"/data/live.err",
		.fault = "/opt/apps/com.samsung."PACKAGE"/data/live.fault",
	},

	.path = {
		.conf = "/opt/live/%s/etc/%s.conf",
		.image = "/opt/share/live_magazine/",
		.script = "/opt/live/%s/res/script/%s.edj",
		.root = "/opt/live/",
		.script_port = "/opt/live/script_port/",
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
		}
	},

	.max_size_type = 4,
	.quality = "quality=100 compress=1",
	.error = "/opt/apps/com.samsung."PACKAGE"/res/images/error.png",
	.ping_time = 60.0f,
	.slave_max_load = 10,
};

void conf_update_size(void)
{
	ecore_x_window_size_get(0, &g_conf.width, &g_conf.height);
}

/* End of a file */
