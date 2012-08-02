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

#define MAX_SIZE_LIST	5

struct conf {
	int width;
	int height;

	struct {
		const char *conf;
		const char *image;
		const char *script;
		const char *root;
		const char *script_port;
		const char *slave_log;
	} path;

	struct {
		int width;
		int height;
	} size[MAX_SIZE_LIST];

	const int max_size_type;

	const char *quality;
	const char *error;

	const int slave_max_load;

	double ping_time;
	double delayed_ctx_init_time;
};

extern struct conf g_conf;

extern void conf_update_size(void);

#define BASE_W 720
#define BASE_H 1280

#define CR 13
#define LF 10

#define MINIMUM_PERIOD 1.0f

#define DEFAULT_SCRIPT	"edje"
#define DEFAULT_ABI	"c"
#define DEFAULT_GROUP	"disclosure"
#define BUNDLE_SLAVE_NAME "name"
#define BUNDLE_SLAVE_SECURED "secured"
#define BUNDLE_SLAVE_ABI "abi"
#define PACKET_TIME 0.01f
#define NO_CHANGE -1.0f
#define DEFAULT_CONTENT "default"
#define DEFAULT_PERIOD	-1.0f
#define MINIMUM_SPACE	(5 << 20)

#define MAX_LOG_LINE 1000
#define MAX_LOG_FILE 3

#define SQLITE_FLUSH_MAX 1048576

/* End of a file */
