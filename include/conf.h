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

#define MAX_SIZE_LIST	6

struct conf {
	int width;
	int height;

	int base_width;
	int base_height;
	double minimum_period;

	struct {
		const char *script;
		const char *abi;
		const char *pd_group;
		double period;
	} default_conf;

	struct {
		const char *name;
		const char *secured;
		const char *abi;
	} launch_key;


	double default_packet_time;

	const char *empty_content;
	const char *empty_title;

	const char *default_content;
	const char *default_title;

	unsigned long minimum_space;

	const char *replace_tag;

	double slave_ttl;

	int max_log_line;
	int max_log_file;

	unsigned long sqlite_flush_max;

	struct {
		const char *conf;
		const char *image;
		const char *script;
		const char *root;
		const char *script_port;
		const char *slave_log;
		const char *db;
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

	const char *vconf_sys_cluster;
};

extern struct conf g_conf;

extern void conf_update_size(void);

#define BASE_W			g_conf.base_width
#define BASE_H			g_conf.base_height

#define CR 13
#define LF 10

#define MINIMUM_PERIOD		g_conf.minimum_period

#define DEFAULT_SCRIPT		g_conf.default_conf.script
#define DEFAULT_ABI		g_conf.default_conf.abi
#define DEFAULT_GROUP		g_conf.default_conf.pd_group
#define NO_CHANGE		g_conf.default_conf.period
#define DEFAULT_PERIOD		g_conf.default_conf.period

#define BUNDLE_SLAVE_NAME	g_conf.launch_key.name
#define BUNDLE_SLAVE_SECURED	g_conf.launch_key.secured
#define BUNDLE_SLAVE_ABI	g_conf.launch_key.abi
#define PACKET_TIME		g_conf.default_packet_time
#define CONTENT_NO_CHANGE	g_conf.empty_content
#define TITLE_NO_CHANGE		g_conf.empty_title
#define DEFAULT_TITLE		g_conf.default_title
#define DEFAULT_CONTENT		g_conf.default_content
#define MINIMUM_SPACE		g_conf.minimum_space

#define IMAGE_PATH		g_conf.path.image
#define SCRIPT_PATH		g_conf.path.script
#define SCRIPT_PORT_PATH	g_conf.path.script_port
#define CONF_PATH		g_conf.path.conf
#define ROOT_PATH		g_conf.path.root
#define SLAVE_LOG_PATH		g_conf.path.slave_log

#define REPLACE_TAG_APPID	g_conf.replace_tag
#define SLAVE_TTL		g_conf.slave_ttl

#define MAX_LOG_LINE		g_conf.max_log_line
#define MAX_LOG_FILE		g_conf.max_log_file

#define SQLITE_FLUSH_MAX	g_conf.sqlite_flush_max
#define DBFILE			g_conf.path.db

#define DEFAULT_QUALITY		g_conf.quality

#define SLAVE_MAX_LOAD		g_conf.slave_max_load
#define DEFAULT_PING_TIME	g_conf.ping_time

#define MAX_ABI		256
#define MAX_PKGNAME	512

#define SYS_CLUSTER_KEY		g_conf.vconf_sys_cluster

/* End of a file */
