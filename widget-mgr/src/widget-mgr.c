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

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <ctype.h>
#include <bundle.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XShm.h>
#include <X11/Xutil.h>

#include <glib.h>
#include <glib-object.h>

#include <packet.h>
#include <com-core_packet.h>
#include <com-core.h>

#include <widget_errno.h>
#include <widget_service.h>
#include <widget_service_internal.h>

#include <Ecore.h>

#include "widget-info.h"
#include "node.h"

#define PROMPT "widget-mgr "
#define PROVIDER_FOLDER "provider"
#define PACKAGE_FOLDER "package"

struct package {
	int primary;
	char *pkgid;

	int pid;
	char *slavename;
	char *abi;
	int refcnt;
	int fault_count;
	int inst_count;
};

struct instance {
	char *id;
	char *buf_id;
	char *cluster;
	char *category;
	double period;
	char *state;
	int width;
	int height;
};

struct slave {
	int pid;
	char *pkgname;
	char *abi;
	int secured;
	int refcnt;
	int fault_count;
	char *state;
	int loaded_inst;
	int loaded_pkg;
	double ttl;
};

enum command {
	NOP,
	PKG_LIST,
	INST_LIST,
	SLAVE_LIST,	
	INST_CTRL,
	SLAVE_CTRL,
	MASTER_CTRL,
};

static struct info {
	int fifo_handle;
	int fd;
	Ecore_Fd_Handler *fd_handler;
	Ecore_Fd_Handler *in_handler;

	struct node *rootdir;
	struct node *curdir;
	struct node *targetdir;

	enum command cmd;

	int input_fd;
	int verbose;

	int age;

	char *history[1024];
	int history_top;
	int history_idx;

	struct node *quick_search_node;
	int quick_idx;

	int dump;
} s_info = {
	.fifo_handle = -EINVAL,
	.fd = -EINVAL,
	.fd_handler = NULL,
	.in_handler = NULL,
	.rootdir = NULL,
	.curdir = NULL,
	.targetdir = NULL,
	.cmd = NOP,
	.input_fd = STDIN_FILENO,
	.verbose = 0,
	.age = 0,
	.history = { 0, },
	.history_top = 0,
	.history_idx = 0,
	.quick_search_node = NULL,
	.quick_idx = 0,
	.dump = 0,
};

char *optarg;
int errno;
int optind;
int optopt;
int opterr;

static Eina_Bool input_cb(void *data, Ecore_Fd_Handler *fd_handler);

static inline const char *trim_cmd(const char *cmd)
{
	while (*cmd && *cmd == ' ') cmd++;

	return cmd;
}

static Eina_Bool process_line_cb(void *data)
{
	input_cb(NULL, NULL);
	return ECORE_CALLBACK_CANCEL;
}

static inline void prompt(const char *cmdline)
{
	char *path;

	if (s_info.input_fd != STDIN_FILENO) {
		/* To prevent recursive call, add function to the main loop (idler) */
		ecore_idler_add(process_line_cb, NULL);
		return;
	}

	path = node_to_abspath(s_info.curdir);
	printf(PROMPT"%s # %s", path, cmdline ? cmdline : "");
	free(path);
}

static void provider_del_cb(struct node *node)
{
	struct slave *info;

	info = node_data(node);
	if (!info) {
		return;
	}

	free(info->pkgname);
	free(info->abi);
	free(info->state);
	free(info);
}

static void package_del_cb(struct node *node)
{
	struct package *info;

	info = node_data(node);
	if (!info) {
		return;
	}

	free(info->pkgid);
	free(info->slavename);
	free(info->abi);
	free(info);
}

static void inst_del_cb(struct node *node)
{
	struct instance *info;

	info = node_data(node);
	if (!info) {
		return;
	}

	free(info->id);
	free(info->buf_id);
	free(info->cluster);
	free(info->category);
	free(info->state);
	free(info);
}

static void ls(void)
{
	struct node *node;
	int cnt = 0;
	int is_package;
	int is_provider;
	int is_instance;
	struct node *next_node;

	if (!(node_mode(s_info.targetdir) & NODE_READ)) {
		printf("Access denied\n");
		return;
	}

	is_package = node_name(s_info.targetdir) && !strcmp(node_name(s_info.targetdir), PACKAGE_FOLDER);
	is_provider = !is_package && node_name(s_info.targetdir) && !strcmp(node_name(s_info.targetdir), PROVIDER_FOLDER);
	is_instance = !is_package && !is_provider && node_parent(s_info.targetdir) && node_name(node_parent(s_info.targetdir)) && !strcmp(node_name(node_parent(s_info.targetdir)), PACKAGE_FOLDER);

	node = node_child(s_info.targetdir);
	while (node) {
		if (is_package) {
			struct package *info;

			next_node = node_next_sibling(node);
			if (node_age(node) != s_info.age) {
				node_delete(node, package_del_cb);
				node = next_node;
				continue;
			}

			info = node_data(node);
			printf(" %3d %20s %5s ", info->inst_count, info->slavename ? info->slavename : "(none)", info->abi ? info->abi : "?");
		} else if (is_provider) {
			struct slave *info;

			next_node = node_next_sibling(node);
			if (node_age(node) != s_info.age) {
				node_delete(node, provider_del_cb);
				node = next_node;
				continue;
			}

			info = node_data(node);
			printf("%6d %3d %5s %5.2f ", info->pid, info->loaded_inst, info->abi ? info->abi : "?", info->ttl);
		} else if (is_instance) {
			struct instance *info;
			struct stat stat;
			char buf[4096];

			next_node = node_next_sibling(node);

			if (node_age(node) != s_info.age) {
				node_delete(node, inst_del_cb);
				node = next_node;
				continue;
			}

			info = node_data(node);

			printf(" %5.2f %s %6s %10s %10s %4dx%-4d ", info->period, info->buf_id, info->state, info->cluster, info->category, info->width, info->height);
			snprintf(buf, sizeof(buf), "/opt/usr/share/live_magazine/reader/%s", node_name(node));
			if (lstat(buf, &stat) < 0) {
				printf("%3d ERR ", errno);
			} else {
				printf("%2.2lf KB ", (double)stat.st_size / 1024.0f);
			}
		}

		if (node_type(node) == NODE_DIR) {
			printf("%s/", node_name(node));
		} else if (node_type(node) == NODE_FILE) {
			printf("%s", node_name(node));
		}

		printf("\n");
		node = node_next_sibling(node);
		cnt++;
	}

	printf("Total: %d\n", cnt);
}

static void send_slave_list(void)
{
	struct packet *packet;
	int ret;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("slave_list", "d", 0.0f);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to send a packet: %d\n", ret);
		return;
	}

	s_info.cmd = SLAVE_LIST;
	s_info.age++;
}

/*!
 * var = debug, slave_max_load
 * cmd = set / get
 */
static void send_command(const char *cmd, const char *var, const char *val)
{
	struct packet *packet;
	int ret;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("master_ctrl", "sss", cmd, var, val);
	if (!packet) {
		printf("Failed to create a ctrl packet\n");
		return;
	}

	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to send packet ctrl\n");
		return;
	}

	s_info.cmd = MASTER_CTRL;
	s_info.age++;
}

static int pkglist_cb(const char *appid, const char *lbid, int is_prime, void *data)
{
	struct node *parent = data;
	struct node *node;
	struct package *info;

	node = node_find(parent, lbid);
	if (node) {
		info = node_data(node);
		if (!info) {
			printf("Invalid node\n");
			return -EINVAL;
		}

		free(info->pkgid);
		info->pkgid = strdup(appid);
		if (!info->pkgid) {
			printf("strdup: %d\n", errno);
			return -ENOMEM;
		}

		node_set_age(node, s_info.age);
		return 0;
	}

	info = calloc(1, sizeof(*info));
	if (!info) {
		printf("calloc: %d\n", errno);
		return -ENOMEM;
	}

	info->pkgid = strdup(appid);
	if (!info->pkgid) {
		printf("strdup: %d\n", errno);
		free(info);
		return -ENOMEM;
	}

	info->primary = is_prime;

	node = node_create(parent, lbid, NODE_DIR, NODE_READ | NODE_EXEC);
	if (!node) {
		free(info->pkgid);
		free(info);
		return -ENOMEM;
	}

	node_set_data(node, info);
	node_set_age(node, s_info.age);
	return 0;
}

static void send_pkg_list(void)
{
	struct packet *packet;
	int ret;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("pkg_list", "d", 0.0f);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to create a packet\n");
		return;
	}

	s_info.cmd = PKG_LIST;
	s_info.age++;

	widget_service_get_widget_list(pkglist_cb, s_info.targetdir);
}

static void send_inst_delete(void)
{
	struct packet *packet;
	struct node *parent;
	const char *name;
	struct instance *inst;
	int ret;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	parent = node_parent(s_info.targetdir);
	if (!parent) {
		printf("Invalid argument\n");
		return;
	}

	if (!node_parent(parent)) {
		printf("Invalid argument\n");
		return;
	}

	name = node_name(node_parent(parent));
	if (!name || strcmp(name, PACKAGE_FOLDER)) {
		printf("Invalid argument\n");
		return;
	}

	inst = node_data(s_info.targetdir);
	name = node_name(parent);

	packet = packet_create_noack("pkg_ctrl", "sss", "rminst", name, inst->id);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to send a packet: %d\n", ret);
		return;
	}

	s_info.cmd = INST_CTRL;
	s_info.age++;
}

static void send_inst_fault(void)
{
	struct packet *packet;
	struct node *parent;
	const char *name;
	struct instance *inst;
	int ret;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	parent = node_parent(s_info.targetdir);
	if (!parent) {
		printf("Invalid argument\n");
		return;
	}

	if (!node_parent(parent)) {
		printf("Invalid argument\n");
		return;
	}

	name = node_name(node_parent(parent));
	if (!name || strcmp(name, PACKAGE_FOLDER)) {
		printf("Invalid argument\n");
		return;
	}

	inst = node_data(s_info.targetdir);
	name = node_name(parent);

	packet = packet_create_noack("pkg_ctrl", "sss", "faultinst", name, inst->id);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to send a packet: %d\n", ret);
		return;
	}

	s_info.cmd = INST_CTRL;
	s_info.age++;
}

static void send_inst_list(const char *pkgname)
{
	struct packet *packet;
	int ret;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("inst_list", "s", pkgname);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to send a packet: %d\n", ret);
		return;
	}

	s_info.cmd = INST_LIST;
	s_info.age++;
}

static void help(void)
{
	printf("widget-mgr - widget utility\n");
	printf("------------------------------ [Option] ------------------------------\n");
	printf("-b Batch mode\n");
	printf("-x execute command\n");
	printf("------------------------------ [Command list] ------------------------------\n");
	printf("[32mcd [PATH] - Change directory[0m\n");
	printf("[32mls [ | PATH] - List up content as a file[0m\n");
	printf("[32mrm [PKG_ID|INST_ID] - Delete package or instance[0m\n");
	printf("[32mstat [path] - Display the information of given path[0m\n");
	printf("[32mset [debug] [on|off] Set the control variable of master provider[0m\n");
	printf("[32mx damage Pix x y w h - Create damage event for given pixmap[0m\n");
	printf("[32mx move Pix x y - Move the window[0m\n");
	printf("[32mx resize Pix w h - Resize the window[0m\n");
	printf("[32mx map Pix - Show the window[0m\n");
	printf("[32mx unmap Pix - Hide the window[0m\n");
	printf("[32mx capture Pix outfile - Capture pixmap and save it to outfile[0m\n");
	printf("[32msh [command] Execute shell command, [command] should be abspath[0m\n");
	printf("[32mexit - [0m\n");
	printf("[32mquit - [0m\n");
	printf("----------------------------------------------------------------------------\n");
}

static inline void init_directory(void)
{
	struct node *node;
	s_info.rootdir = node_create(NULL, NULL, NODE_DIR, NODE_READ | NODE_EXEC);
	if (!s_info.rootdir) {
		return;
	}

	node = node_create(s_info.rootdir, PROVIDER_FOLDER, NODE_DIR, NODE_READ | NODE_EXEC);
	if (!node) {
		node_destroy(s_info.rootdir);
		s_info.rootdir = NULL;
		return;
	}

	node = node_create(s_info.rootdir, PACKAGE_FOLDER, NODE_DIR, NODE_READ | NODE_EXEC);
	if (!node) {
		node_destroy(node_child(s_info.rootdir));
		node_destroy(s_info.rootdir);
		s_info.rootdir = NULL;
		return;
	}

	s_info.curdir = s_info.rootdir;
	return;
}

static inline void fini_directory(void)
{
}

static struct node *update_target_dir(const char *cmd)
{
	struct node *node;

	node = (*cmd == '/') ? s_info.rootdir : s_info.curdir;
	node = node_find(node, cmd);

	return node;
}

static int get_token(const char *src, char *out)
{
	int len = 0;
	while (*src && *src == ' ') src++;

	if (!*src) {
		return 0;
	}

	while (*src && *src != ' ') {
		*out++ = *src++;
		len++;
	}

	*out = '\0';
	return len;
}

static inline int do_stat(const char *cmd)
{
	int i;
	struct node *node;
	struct node *parent;
	char *tmp;
	enum stat_type {
		PKG_INSTANCE = 0x01,
		PKG,
		PROVIDER_INSTANCE,
		PROVIDER,
		ROOT,
	} type;

	cmd = trim_cmd(cmd + 5);
	if (!*cmd){
		printf("Invalid argument\n");
		return -EINVAL;
	}

	node = node_find(*cmd == '/' ? s_info.rootdir : s_info.curdir, cmd);
	if (!node) {
		printf("Invalid path\n");
		return -EINVAL;
	}

	i = 0;
	type = ROOT;
	parent = node_parent(node);
	while (parent) {
		if (!node_name(parent)) {
			printf("%s has no info\n", node_name(node));
			return -EINVAL;
		} else if (!strcmp(node_name(parent), PACKAGE_FOLDER)) {
			type = (i == 0) ? PKG : PKG_INSTANCE;
			break;
		} else if (!strcmp(node_name(parent), PROVIDER_FOLDER)){
			type = (i == 0) ? PROVIDER : PROVIDER_INSTANCE;
			break;
		}

		parent = node_parent(parent);
		i++;
		if (i > 1) {
			printf("%s is invalid path\n", node_name(node));
			return -EINVAL;
		}
	}

	switch (type){
	case PKG:
		tmp = widget_service_get_name(node_name(node), NULL);
		printf("Name: %s (", tmp);
		free(tmp);

		i = widget_service_is_enabled(node_name(node));
		printf("%s)\n", i ? "enabled" : "disabled");

		tmp = widget_service_get_icon(node_name(node), NULL);
		printf("Icon: %s\n", tmp);
		free(tmp);

		tmp = widget_service_get_category(node_name(node));
		printf("Category: %s\n", tmp);
		free(tmp);

		tmp = widget_service_get_provider_name(node_name(node));
		printf("Provider: %s (content:", tmp);
		free(tmp);

		tmp = widget_service_get_content_string(node_name(node));
		printf("%s)\n", tmp);
		free(tmp);

		tmp = widget_service_get_widget_script_path(node_name(node));
		printf("LB Script: %s (", tmp);
		free(tmp);

		tmp = widget_service_get_widget_script_group(node_name(node));
		printf("%s)\n", tmp);
		free(tmp);

		tmp = widget_service_get_gbar_script_path(node_name(node));
		printf("PD Script: %s (", tmp);
		free(tmp);

		tmp = widget_service_get_gbar_script_group(node_name(node));
		printf("%s)\n", tmp);
		free(tmp);

		break;
	case PROVIDER:
		printf("Not supported yet\n");
		break;
	case PKG_INSTANCE:
		printf("Not supported yet\n");
		break;
	case PROVIDER_INSTANCE:
		printf("Not supported yet\n");
		break;
	case ROOT:
		printf("Not supported yet\n");
		break;
	}

	return 0;
}

static int do_set(const char *cmd)
{
	int i;
	char variable[4096] = { '0', };

	cmd += 4;
	i = get_token(cmd, variable);

	cmd = trim_cmd(cmd + i);
	if (!i || !*cmd) {
		printf("Invalid argument(%s): set [VAR] [VAL]\n", cmd);
		return -EINVAL;
	}

	send_command("set", variable, cmd);
	return 0;
}

static inline int do_get(const char *cmd)
{
	cmd = trim_cmd(cmd + 4);
	if (!*cmd) {
		printf("Invalid argument(%s): get [VAR]\n", cmd);
		return -EINVAL;
	}

	send_command("get", cmd, "");
	return 0;
}

static inline int do_ls(const char *cmd)
{
	const char *name;
	struct node *parent;

	cmd = trim_cmd(cmd + 2);

	s_info.targetdir = *cmd ? update_target_dir(cmd) : s_info.curdir;
	if (!s_info.targetdir) {
		printf("%s is not exists\n", cmd);
		return -ENOENT;
	}

	name = node_name(s_info.targetdir);
	if (name) {
		if (!strcmp(name, PACKAGE_FOLDER)) {
			if (s_info.cmd == NOP) {
				send_pkg_list();
				return 0;
			}

			printf("Waiting the server response\n");
			return -EBUSY;
		} else if (!strcmp(name, PROVIDER_FOLDER)) {
			if (s_info.cmd == NOP) {
				send_slave_list();
				return 0;
			}

			printf("Waiting the server response\n");
			return -EBUSY;
		}
	}

	parent = node_parent(s_info.targetdir);
	if (parent && node_name(parent)) {
		if (!strcmp(node_name(parent), PACKAGE_FOLDER)) {
			if (s_info.cmd != NOP) {
				printf("Waiting the server response\n");
				return -EBUSY;
			}

			send_inst_list(name);
			return 0;
		}
	}

	ls();
	return -1;
}

static inline int do_cd(const char *cmd)
{
	cmd = trim_cmd(cmd + 2);
	if (!*cmd) {
		return -1;
	}

	if (s_info.cmd != NOP) {
		printf("Waiting the server response\n");
		return -EBUSY;
	}

	s_info.targetdir = update_target_dir(cmd);
	if (!s_info.targetdir) {
		printf("%s is not exists\n", cmd);
		return -ENOENT;
	}

	if (node_type(s_info.targetdir) != NODE_DIR) {
		printf("Unable change directory to %s\n", cmd);
		return -EINVAL;
	}

	if (!(node_mode(s_info.targetdir) & NODE_EXEC)) {
		printf("Access denied %s\n", cmd);
		return -EACCES;
	}

	s_info.curdir = s_info.targetdir;
	return -1;
}

static inline int do_rm(const char *cmd)
{
	cmd = trim_cmd(cmd + 2);
	if (!*cmd) {
		return -1;
	}

	if (s_info.cmd != NOP) {
		printf("Waiting the server response\n");
		return -EBUSY;
	}

	s_info.targetdir = update_target_dir(cmd);
	if (!s_info.targetdir) {
		printf("%s is not exists\n", cmd);
		return -ENOENT;
	}

	if (!(node_mode(s_info.targetdir) & NODE_WRITE)) {
		printf("Access denied %s\n", cmd);
		return -EACCES;
	}

	send_inst_delete();
	return 0;
}

static inline int do_fault(const char *cmd)
{
	cmd = trim_cmd(cmd + 5);
	if (!*cmd) {
		return -1;
	}

	if (s_info.cmd != NOP) {
		printf("Waiting the server response\n");
		return -EBUSY;
	}

	s_info.targetdir = update_target_dir(cmd);
	if (!s_info.targetdir) {
		printf("%s is not exists\n", cmd);
		return -ENOENT;
	}

	if (!(node_mode(s_info.targetdir) & NODE_WRITE)) {
		printf("Access denied %s\n", cmd);
		return -EACCES;
	}

	send_inst_fault();
	return 0;
}

#if !defined(WCOREDUMP)
#define WCOREDUMP(a)	0
#endif

static void do_sh(const char *cmd)
{
	pid_t pid;

	cmd = trim_cmd(cmd + 3);
	if (!*cmd) {
		return;
	}

	pid = fork();
	if (pid == 0) {
		char command[256];
		int idx;
		idx = 0;

		while (idx < (sizeof(command) - 1) && *cmd && *cmd != ' ') {
			command[idx++] = *cmd++;
		}
		command[idx] = '\0';

		if (execl(command, cmd, NULL) < 0) {
			printf("execl: %d\n", errno);
		}

		exit(0);
	} else if (pid < 0) {
		printf("Failed to create a new process: %d\n", errno);
	} else {
		int status;
		if (waitpid(pid, &status, 0) < 0) {
			printf("waitpid: %d\n", errno);
		} else {
			if (WIFEXITED(status)) {
				printf("Exit: %d\n", WEXITSTATUS(status));
			} else if (WIFSIGNALED(status)) {
				printf("Terminated by %d %s\n", WTERMSIG(status), WCOREDUMP(status) ? " - core generated" : "");
			} else if (WIFSTOPPED(status)) {
				printf("Stopped by %d\n", WSTOPSIG(status));
			} else if (WIFCONTINUED(status)) {
				printf("Child is resumed\n");
			}
		}
	}
}

static inline int get_pixmap_size(Display *disp, Pixmap id, int *x, int *y, unsigned int *w, unsigned int *h)
{
	Window dummy_win;
	unsigned int dummy_border, dummy_depth;
	int _x;
	int _y;

	if (!x) {
		x = &_x;
	}

	if (!y) {
		y = &_y;
	}

	if (!XGetGeometry(disp, id, &dummy_win, x, y, w, h, &dummy_border, &dummy_depth)) {
		return -EFAULT;
	}

	return 0;
}

static inline int do_capture(Display *disp, Pixmap id, const char *filename)
{
	XShmSegmentInfo si;
	XImage *xim;
	Visual *visual;
	unsigned int w;
	unsigned int h;
	int bufsz;
	int fd;
	Screen *screen;

	screen = DefaultScreenOfDisplay(disp);
	visual = DefaultVisualOfScreen(screen);

	if (get_pixmap_size(disp, id, NULL, NULL, &w, &h) < 0) {
		printf("Failed to get size of a pixmap\n");
		return -EINVAL;
	}

	printf("Pixmap size: %dx%d\n", w, h);
	bufsz = w * h * sizeof(int);

	si.shmid = shmget(IPC_PRIVATE, bufsz, IPC_CREAT | 0666);
	if (si.shmid < 0) {
		printf("shmget: %d\n", errno);
		return -EFAULT;
	}

	si.readOnly = False;
	si.shmaddr = shmat(si.shmid, NULL, 0);
	if (si.shmaddr == (void *)-1) {

		if (shmctl(si.shmid, IPC_RMID, 0) < 0) {
			printf("shmctl: %d\n", errno);
		}

		return -EFAULT;
	}

	/*!
	 * \NOTE
	 * Use the 24 bits Pixmap for Video player
	 */
	xim = XShmCreateImage(disp, visual, 24 /* (depth << 3) */, ZPixmap, NULL, &si, w, h);
	if (xim == NULL) {
		if (shmdt(si.shmaddr) < 0) {
			printf("shmdt: %d\n", errno);
		}

		if (shmctl(si.shmid, IPC_RMID, 0) < 0) {
			printf("shmctl: %d\n", errno);
		}

		return -EFAULT;
	}

	xim->data = si.shmaddr;
	XShmAttach(disp, &si);

	XShmGetImage(disp, id, xim, 0, 0, 0xFFFFFFFF);
	XSync(disp, False);

	fd = open(filename, O_CREAT | O_RDWR, 0644);
	if (fd >= 0) {
		if (write(fd, xim->data, bufsz) != bufsz) {
			printf("Data is not fully written\n");
		}

		if (close(fd) < 0) {
			printf("close: %d\n", errno);
		}
	} else {
		printf("Error: %d\n", errno);
	}

	XShmDetach(disp, &si);
	XDestroyImage(xim);

	if (shmdt(si.shmaddr) < 0) {
		printf("shmdt: %d\n", errno);
	}

	if (shmctl(si.shmid, IPC_RMID, 0) < 0) {
		printf("shmctl: %d\n", errno);
	}

	return 0;
}

static int widget_lifecycle_event(const char *widget_id, widget_lifecycle_event_e ev, const char *instance_id, void *data)
{
	printf("[%s] [%s] [STATE: 0x%X]\n", widget_id, instance_id, (unsigned int)ev);
	return WIDGET_ERROR_NONE;
}

static void do_monitor(const char *cmd)
{
	cmd = trim_cmd(cmd + strlen("monitor"));

	if (!*cmd) {
		/* Monitor all instances */
	} else {
		/* Monitor one */
		printf("Instance = [%s]\n", cmd);
	}

	widget_service_set_lifecycle_event_cb(*cmd ? cmd : NULL, widget_lifecycle_event, NULL);
}

static int widget_inst_list_cb(const char *widget_id, const char *instance_id, void *data)
{
	printf("Instance: %s\n", instance_id);
	return WIDGET_ERROR_NONE;
}

static void do_get_list(const char *cmd)
{
	cmd = trim_cmd(cmd + strlen("get_list"));
	if (!*cmd) {
		return;
	}

	printf("Instance list of %s\n", cmd);
	if (widget_service_get_widget_instance_list(cmd, widget_inst_list_cb, NULL) == WIDGET_ERROR_NONE) {
		printf("=== Done\n");
	} else {
		printf("Failed to get instance list\n");
	}
}

static int do_frame_dump(const char *cmd)
{
	struct instance *info;
	struct packet *packet;
	struct node *parent;
	char widget_id[256];
	int value;
	int ret;

	cmd = trim_cmd(cmd + strlen("frame_dump"));
	if (!*cmd) {
		return -EINVAL;
	}

	if (s_info.cmd != NOP) {
		printf("Waiting the server response\n");
		return -EBUSY;
	}

	if (sscanf(cmd, "%[^ ] %d", widget_id, &value) != 2) {
		printf("Error[%s]\n", cmd);
		return -EINVAL;
	}

	s_info.targetdir = update_target_dir(widget_id);
	if (!s_info.targetdir) {
		printf("%s is not exists\n", cmd);
		return -ENOENT;
	}

	if (!(node_mode(s_info.targetdir) & NODE_WRITE)) {
		printf("Access denied %s\n", cmd);
		return -EACCES;
	}

	parent = node_parent(s_info.targetdir);
	if (!parent) {
		printf("Invalid folder: %s\n", cmd);
		return -EFAULT;
	}

	info = node_data(s_info.targetdir);
	printf("WidgetId: %s\n", info->id);
	packet = packet_create_noack("slave_ctrl", "ssii", node_name(parent), info->id, WIDGET_CTRL_MODE_DUMP_FRAME, !!value);
	ret = com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	if (ret < 0) {
		printf("Failed to send a packet: %d\n", ret);
		return ret;
	}

	printf("Dump frame %s (%s)\n", cmd, node_name(parent));
	s_info.age++;

	return 0;
}

static void do_get_content(const char *cmd)
{
	char widget_id[256];
	char inst_id[256];
	cmd = trim_cmd(cmd + strlen("get_content"));
	if (!*cmd) {
		return;
	}

	if (sscanf(cmd, "%[^ ] %s", widget_id, inst_id) == 2) {
		bundle *b;
		if (widget_service_get_content_of_widget_instance(widget_id, inst_id, &b) == WIDGET_ERROR_NONE) {
			int len;
			char *content = NULL;

			if (b && bundle_encode(b, (bundle_raw **)&content, &len) == BUNDLE_ERROR_NONE) {
				printf("Widget: %s\nInstance: %s\nContent: [%s]\n", widget_id, inst_id, content);
				bundle_free(b);
			} else {
				printf("Widget: %s\nInstance: %s\nbundle: %p\n", widget_id, inst_id, b);
			}

			free(content);
		} else {
			printf("Failed to get content info for %s\n", inst_id);
		}
	}
}

static void do_demonitor(const char *cmd)
{
	cmd = trim_cmd(cmd + strlen("demonitor"));
	if (!*cmd) {
	} else {
		printf("Instance = [%s]\n", cmd);
	}

	widget_service_unset_lifecycle_event_cb(*cmd ? cmd : NULL, NULL);
}

static void do_dump(const char *cmd)
{
	char path[256];
	int idx;
	struct node *node;
	struct node *curdir;
	struct node *target;

	cmd = trim_cmd(cmd + 5);
	if (!*cmd) {
		return;
	}

	curdir = s_info.curdir;
	do {
		target = NULL;
		idx = 0;
		while (cmd[idx] != '/' && cmd[idx] != '\0' && cmd[idx] != ' ') {
			idx++;
		}

		if (!idx) {
			break;
		}

		strncpy(path, cmd, idx);
		path[idx] = '\0';
		cmd += idx;

		node = node_child(curdir);
		while (node) {
			if (!strcmp(node_name(node), path)) {
				curdir = node;
				target = curdir;
				break;
			}

			node = node_next_sibling(node);
		}

		if (*cmd != '/') {
			break;
		}
	} while ((*++cmd));

	if (target && node_type(target) != NODE_DIR) {
		struct instance *instinfo;

		if (node_type(target) == NODE_LINK) {
			/* Follow up node */
		}

		instinfo = node_data(target);
		if (instinfo) {
			while (*cmd == ' ') {
				cmd++;
			}

			if (sscanf(cmd, "%255[^ ]", path) != 1) {
				printf("dump file OUT_FILENAME\n");
				return;
			}

			if (instinfo->buf_id) {
				unsigned int pixmap;
				int pixel_size;
				if (sscanf(instinfo->buf_id, "pixmap://%u:%d", &pixmap, &pixel_size) == 2) {
					Display *disp;

					disp = XOpenDisplay(NULL);
					if (!disp) {
						printf("Failed to connect to the X\n");
						return;
					}

					if (do_capture(disp, (Pixmap)pixmap, path) == 0) {
						printf("Captured: %s\n", path);
					}
					XCloseDisplay(disp);
				} else {
					printf("Unsupported type\n");
				}
			} else {
				printf("Buffer is not exists\n");
			}
		}
	} else {
		printf("target only can be a filename\n");
	}
}

static void do_x(const char *cmd)
{
	Display *disp;

	cmd = trim_cmd(cmd + 2);
	if (!*cmd) {
		return;
	}

	disp = XOpenDisplay(NULL);
	if (!disp) {
		printf("Failed to connect to the X\n");
		return;
	}

	if (!strncasecmp(cmd, "damage ", 7)) {
		unsigned int winId;
		XRectangle rect;
		XserverRegion region;
		int x, y, w, h;

		cmd += 7;

		if (sscanf(cmd, "%u %d %d %d %d", &winId, &x, &y, &w, &h) != 5) {
			printf("Invalid argument\nx damage WINID_DEC X Y W H\n");
			return;
		}
		rect.x = x;
		rect.y = y;
		rect.width = w;
		rect.height = h;
		region = XFixesCreateRegion(disp, &rect, 1);
		XDamageAdd(disp, winId, region);
		XFixesDestroyRegion(disp, region);
		XFlush(disp);

		printf("Damage: %u %d %d %d %d\n", winId, x, y, w, h);
	} else if (!strncasecmp(cmd, "capture ", 8)) {
		unsigned int winId;
		char filename[256];

		cmd += 8;

		if (sscanf(cmd, "%u %255[^ ]", &winId, filename) != 2) {
			printf("Invalid argument\nx capture WINID_DEC FILENAME (%s)\n", cmd);
			return;
		}
		if (do_capture(disp, winId, filename) == 0) {
			printf("Captured: %s\n", filename);
		}
	} else if (!strncasecmp(cmd, "resize ", 7)) {
		unsigned int winId;
		int w;
		int h;

		cmd += 7;

		if (sscanf(cmd, "%u %d %d", &winId, &w, &h) != 3) {
			printf("Invalid argument\nx resize WINID_DEC W H\n");
			return;
		}

		XResizeWindow(disp, winId, w, h);
		printf("Resize: %u %d %d\n", winId, w, h);
	} else if (!strncasecmp(cmd, "move ", 5)) {
		unsigned int winId;
		int x;
		int y;

		cmd += 5;
		if (sscanf(cmd, "%u %d %d", &winId, &x, &y) != 3) {
			printf("Invalid argument\nx move WINID_DEC X Y\n");
			return;
		}

		XMoveWindow(disp, winId, x, y);
		printf("Move: %u %d %d\n", winId, x, y);
	} else if (!strncasecmp(cmd, "map ", 4)) {
		unsigned int winId;
		cmd += 4;
		if (sscanf(cmd, "%u", &winId) != 1) {
			printf("Invalid argument\nx map WINID_DEC\n");
			return;
		}
		XMapRaised(disp, winId);
		printf("Map: %u\n", winId);
	} else if (!strncasecmp(cmd, "unmap ", 6)) {
		unsigned int winId;
		cmd += 6;
		if (sscanf(cmd, "%u", &winId) != 1) {
			printf("Invalid argument\nx unmap WINID_DEC\n");
			return;
		}
		XUnmapWindow(disp, winId);
		printf("Unmap: %u\n", winId);
	} else {
		printf("Unknown command\n");
	}

	XCloseDisplay(disp);
}

static inline void put_command(const char *cmd)
{
	if (s_info.history[s_info.history_top]) {
		free(s_info.history[s_info.history_top]);
		s_info.history[s_info.history_top] = NULL;
	}

	s_info.history[s_info.history_top] = strdup(cmd);
	s_info.history_top = (s_info.history_top + !!s_info.history[s_info.history_top]) % (sizeof(s_info.history) / sizeof(s_info.history[0]));
}

static inline const char *get_command(int idx)
{
	idx = s_info.history_top + idx;
	while (idx < 0) {
		idx += (sizeof(s_info.history) / sizeof(s_info.history[0]));
	}

	return s_info.history[idx];
}

static void do_command(const char *cmd)
{
	/* Skip the first spaces */
	cmd = trim_cmd(cmd);

	if (strlen(cmd) && *cmd != '#') {
		if (!strncasecmp(cmd, "exit", 4) || !strncasecmp(cmd, "quit", 4)) {
			ecore_main_loop_quit();
		} else if (!strncasecmp(cmd, "set ", 4)) {
			if (do_set(cmd) == 0) {
				return;
			}
		} else if (!strncasecmp(cmd, "stat ", 5)) {
			do_stat(cmd);
		} else if (!strncasecmp(cmd, "get ", 4)) {
			if (do_get(cmd) == 0) {
				return;
			}
		} else if (!strncasecmp(cmd, "ls", 2)) {
			if (do_ls(cmd) == 0) {
				return;
			}
		} else if (!strncasecmp(cmd, "cd", 2)) {
			if (do_cd(cmd) == 0) {
				return;
			}
		} else if (!strncasecmp(cmd, "rm", 2)) {
			if (do_rm(cmd) == 0) {
				return;
			}
		} else if (!strncasecmp(cmd, "fault", 5)) {
			if (do_fault(cmd) == 0) {
				return;
			}
		} else if (!strncasecmp(cmd, "sh ", strlen("sh "))) {
			do_sh(cmd);
		} else if (!strncasecmp(cmd, "x ", strlen("x "))) {
			do_x(cmd);
		} else if (!strncasecmp(cmd, "dump", strlen("dump"))) {
			do_dump(cmd);
		} else if (!strncasecmp(cmd, "monitor", strlen("monitor"))) {
			do_monitor(cmd);
		} else if (!strncasecmp(cmd, "demonitor", strlen("demonitor"))) {
			do_demonitor(cmd);
		} else if (!strncasecmp(cmd, "get_content", strlen("get_content"))) {
			do_get_content(cmd);
		} else if (!strncasecmp(cmd, "get_list", strlen("get_list"))) {
			do_get_list(cmd);
		} else if (!strncasecmp(cmd, "frame_dump", strlen("frame_dump"))) {
			do_frame_dump(cmd);
		} else {
			help();
		}
	}

	prompt(NULL);
	return;
}

#define CMD_BUFFER_SIZE 256

static Eina_Bool input_cb(void *data, Ecore_Fd_Handler *fd_handler)
{
	static int idx = 0;
	static char cmd_buffer[CMD_BUFFER_SIZE];
	unsigned char ch;
	int fd;
	int ret;
	const char escape_str[] = { 0x1b, 0x5b, 0x0 };
	const char *escape_ptr = escape_str;
	const char *tmp;

	if (fd_handler) {
		fd = ecore_main_fd_handler_fd_get(fd_handler);
		if (fd < 0) {
			printf("FD is not valid: %d\n", fd);
			return ECORE_CALLBACK_CANCEL;
		}
	} else {
		fd = s_info.input_fd;
	}

	/*!
	 * \note
	 * Using this routine, we can implement the command recommend algorithm.
	 * When a few more characters are matched with history of command, we can show it to user
	 * Then the user will choose one or write new command
	 */

	/* Silly.. Silly */
	while ((ret = read(fd, &ch, sizeof(ch))) == sizeof(ch)) {
		if (*escape_ptr == '\0') {
			/* Function key */
			switch (ch) {
			case 0x41: /* UP */
				printf("%s2K%s1G", escape_str, escape_str);
				tmp = get_command(--s_info.history_idx);
				if (!tmp) {
					s_info.history_idx = 0;
					cmd_buffer[0] = '\0';
					prompt(NULL);
				} else {
					strncpy(cmd_buffer, tmp, CMD_BUFFER_SIZE - 1);
					cmd_buffer[CMD_BUFFER_SIZE - 1]  = '\0';
					idx = strlen(cmd_buffer);
					prompt(cmd_buffer);
				}
				break;
			case 0x42: /* DOWN */
				if (s_info.history_idx >= 0) {
					break;
				}

				printf("%s2K%s1G", escape_str, escape_str);
				tmp = get_command(++s_info.history_idx);
				if (s_info.history_idx == 0) {
					s_info.history_idx = 0;
					cmd_buffer[0] = '\0';
					prompt(NULL);
				} else {
					strncpy(cmd_buffer, tmp, CMD_BUFFER_SIZE - 1);
					cmd_buffer[CMD_BUFFER_SIZE - 1]  = '\0';
					idx = strlen(cmd_buffer);
					prompt(cmd_buffer);
				}
				break;
			case 0x43: /* RIGHT */
				break;
			case 0x44: /* LEFT */
				break;
			default:
				break;
			}

			escape_ptr = escape_str;
			continue;
		} else if (ch == *escape_ptr) {
			escape_ptr++;
			continue;
		}

		switch (ch) {
		case 0x7F: /* BKSP */
		case 0x80:
			cmd_buffer[idx] = '\0';
			if (idx > 0) {
				idx--;
				cmd_buffer[idx] = ' ';
				putc('\r', stdout);
				prompt(cmd_buffer);
			}

			cmd_buffer[idx] = '\0';
			putc('\r', stdout);
			prompt(cmd_buffer);
			break;
		case 0x09: /* TAB */
			if (!s_info.quick_search_node) {
				s_info.quick_search_node = node_child(s_info.curdir);
				s_info.quick_idx = idx;
			} else {
				s_info.quick_search_node = node_next_sibling(s_info.quick_search_node);
				idx = s_info.quick_idx;
			}

			if (!s_info.quick_search_node) {
				break;
			}

			printf("%s2K%s1G", escape_str, escape_str);
			strcpy(cmd_buffer + idx, node_name(s_info.quick_search_node));
			idx += strlen(node_name(s_info.quick_search_node));
			prompt(cmd_buffer);
			break;
		case '\n':
		case '\r':
			cmd_buffer[idx] = '\0';
			idx = 0;
			if (s_info.input_fd == STDIN_FILENO || s_info.verbose) {
				putc((int)'\n', stdout);
			}
			do_command(cmd_buffer);
			put_command(cmd_buffer);
			memset(cmd_buffer, 0, sizeof(cmd_buffer));
			s_info.history_idx = 0;
			s_info.quick_search_node = NULL;

			/* Make a main loop processing for command handling */
			return ECORE_CALLBACK_RENEW;
		default:
			if (isascii(ch)) {
				cmd_buffer[idx++] = ch;

				if (s_info.input_fd == STDIN_FILENO || s_info.verbose) {
					putc((int)ch, stdout);
				}

				if (idx == sizeof(cmd_buffer) - 1) {
					cmd_buffer[idx] = '\0';
					printf("\nCommand buffer is overflow: %s\n", cmd_buffer);
					idx = 0;
				}
			} else {
				printf("Unknown character: 0x%X\n", (unsigned int)ch);
			}
			break;
		}
	}

	if (ret < 0 && !fd_handler) {
		ecore_main_loop_quit();
	}

	return ECORE_CALLBACK_RENEW;
}

static void processing_line_buffer(const char *buffer)
{
	int pid;
	char slavename[256];
	char pkgname[256];
	char abi[256];
	char inst_id[4096];
	char buf_id[256];
	char cluster[256];
	char category[256];
	char str_period[64];
	char state[10];
	int refcnt;
	int fault_count;
	int list_count;
	int loaded_inst;
	int loaded_pkg;
	double ttl;
	int secured;
	double period;
	int width;
	int height;
	struct node *node;
	struct package *pkginfo;
	struct instance *instinfo;
	struct slave *slaveinfo;
	long i;

	switch (s_info.cmd) {
	case PKG_LIST:
		if (sscanf(buffer, "%d %255[^ ] %255[^ ] %255[^ ] %d %d %d", &pid, slavename, pkgname, abi, &refcnt, &fault_count, &list_count) != 7) {
			printf("Invalid format : [%s]\n", buffer);
			return;
		}

		node = node_find(s_info.targetdir, pkgname);
		if (!node) {
			pkginfo = calloc(1, sizeof(*pkginfo));
			if (!pkginfo) {
				printf("calloc: %d\n", errno);
				return;
			}

			pkginfo->pkgid = strdup("conf.file");
			if (!pkginfo->pkgid) {
				printf("strdup: %d\n", errno);
			}

			pkginfo->primary = 1;

			node = node_create(s_info.targetdir, pkgname, NODE_DIR, NODE_READ | NODE_EXEC);
			if (!node) {
				free(pkginfo->pkgid);
				free(pkginfo);
				printf("Failed to create a new node (%s)\n", pkgname);
				return;
			}

			node_set_data(node, pkginfo);
		} else {
			pkginfo = node_data(node);
			if (!pkginfo) {
				printf("Package info is inavlid\n");
				return;
			}

			free(pkginfo->slavename);
			free(pkginfo->abi);

			pkginfo->slavename = NULL;
			pkginfo->abi = NULL;
		}

		node_set_age(node, s_info.age);

		pkginfo->slavename = strdup(slavename);
		if (!pkginfo->slavename) {
			printf("strdup: %d\n", errno);
		}

		pkginfo->abi = strdup(abi);
		if (!pkginfo->abi) {
			printf("strdup: %d\n", errno);
		}

		pkginfo->pid = pid;
		pkginfo->refcnt = refcnt;
		pkginfo->fault_count = fault_count;
		pkginfo->inst_count = list_count;
		break;
	case SLAVE_LIST:
		if (sscanf(buffer, "%d %[^ ] %[^ ] %[^ ] %d %d %d %[^ ] %d %d %lf", &pid, slavename, pkgname, abi, &secured, &refcnt, &fault_count, state, &loaded_inst, &loaded_pkg, &ttl) != 11) {
			printf("Invalid format : [%s]\n", buffer);
			return;
		}
		node = node_find(s_info.targetdir, slavename);
		if (!node) {
			slaveinfo = calloc(1, sizeof(*slaveinfo));
			if (!slaveinfo) {
				printf("Error: %d\n", errno);
				return;
			}

			node = node_create(s_info.targetdir, slavename, NODE_DIR, NODE_READ | NODE_EXEC);
			if (!node) {
				free(slaveinfo);
				return;
			}

			node_set_data(node, slaveinfo);
		} else {
			slaveinfo = node_data(node);
		}

		node_set_age(node, s_info.age);

		free(slaveinfo->pkgname);
		free(slaveinfo->abi);
		free(slaveinfo->state);

		slaveinfo->pkgname = strdup(pkgname);
		if (!slaveinfo->pkgname) {
			printf("strdup: %d\n", errno);
		}

		slaveinfo->abi = strdup(abi);
		if (!slaveinfo->abi) {
			printf("strdup: %d\n", errno);
		}

		slaveinfo->state = strdup(state);
		if (!slaveinfo->state) {
			printf("strdup: %d\n", errno);
		}

		slaveinfo->pid = pid;
		slaveinfo->secured = secured;
		slaveinfo->refcnt = refcnt;
		slaveinfo->fault_count = fault_count;
		slaveinfo->loaded_inst = loaded_inst;
		slaveinfo->loaded_pkg = loaded_pkg;
		slaveinfo->ttl = ttl;
		break;
	case INST_LIST:
		if (sscanf(buffer, "%[^ ] %[^ ] %[^ ] %[^ ] %[^ ] %[^ ] %d %d", inst_id, buf_id, cluster, category, str_period, state, &width, &height) != 8) {
			if (sscanf(buffer, "%[^ ] %[^ ] %[^ ] %[^ ] %[^ ] %d %d", inst_id, cluster, category, str_period, state, &width, &height) != 7) {
				printf("Invalid format : [%s]\n", buffer);
				return;
			} else {
				buf_id[0] = '\0';
			}
		}

		period = strtod(str_period, NULL);

		for (i = strlen(inst_id); i > 0 && inst_id[i] != '/'; i--);
		i += (inst_id[i] == '/');

		node = node_find(s_info.targetdir, inst_id + i);
		if (!node) {
			instinfo = calloc(1, sizeof(*instinfo));
			if (!instinfo) {
				printf("calloc: %d\n", errno);
				return;
			}

			node = node_create(s_info.targetdir, inst_id + i, NODE_FILE, NODE_READ | NODE_WRITE);
			if (!node) {
				free(instinfo);
				return;
			}

			node_set_data(node, instinfo);
		} else {
			instinfo = node_data(node);
		}

		node_set_age(node, s_info.age);

		free(instinfo->id);
		instinfo->id = NULL;
		free(instinfo->buf_id);
		instinfo->buf_id = NULL;
		free(instinfo->cluster);
		instinfo->cluster = NULL;
		free(instinfo->category);
		instinfo->category = NULL;
		free(instinfo->state);
		instinfo->state = NULL;

		instinfo->id = strdup(inst_id);
		if (!instinfo->id) {
			printf("strdup: %d\n", errno);
		}

		instinfo->cluster = strdup(cluster);
		if (!instinfo->cluster) {
			printf("strdup: %d\n", errno);
		}

		instinfo->category = strdup(category);
		if (!instinfo->category) {
			printf("strdup: %d\n", errno);
		}

		instinfo->state = strdup(state);
		if (!instinfo->state) {
			printf("strdup: %d\n", errno);
		}

		if (strlen(buf_id)) {
			instinfo->buf_id = strdup(buf_id);
		}

		instinfo->period = period;
		instinfo->width = width;
		instinfo->height = height;
		break;
	case INST_CTRL:
		sscanf(buffer, "%ld", &i);
		printf("%ld\n", i);
		printf("Result: %ld\n", i);
		break;
	case SLAVE_CTRL:
		sscanf(buffer, "%ld", &i);
		printf("Result: %ld\n", i);
		break;
	case MASTER_CTRL:
		sscanf(buffer, "%ld", &i);
		printf("Result: %ld\n", i);
		break;
	default:
		break;
	}
}

static inline void do_line_command(void)
{
	switch (s_info.cmd) {
	case PKG_LIST:
		ls();
		break;
	case INST_LIST:
		ls();
		break;
	case SLAVE_LIST:
		ls();
		break;
	case INST_CTRL:
		break;
	case SLAVE_CTRL:
		break;
	case MASTER_CTRL:
		break;
	default:
		break;
	}
	prompt(NULL);
}

static Eina_Bool read_cb(void *data, Ecore_Fd_Handler *fd_handler)
{
	int fd;
	static char *line_buffer = NULL;
	static int line_index = 0;
	static int bufsz = 256;
	char ch;

	fd = ecore_main_fd_handler_fd_get(fd_handler);
	if (fd < 0) {
		printf("FD is not valid: %d\n", fd);
		return ECORE_CALLBACK_CANCEL;
	}

	if (read(fd, &ch, sizeof(ch)) != sizeof(ch)) {
		printf("read: %d\n", errno);
		return ECORE_CALLBACK_CANCEL;
	}

	if (!line_buffer) {
		line_index = 0;
		line_buffer = malloc(bufsz);
		if (!line_buffer) {
			printf("malloc: %d\n", errno);
			return ECORE_CALLBACK_CANCEL;
		}
	}	

	if (ch == '\n') { /* End of a line */
		if (line_index == bufsz - 1) {
			char *new_buf;
			new_buf = realloc(line_buffer, bufsz + 2);
			if (!new_buf) {
				printf("realloc: %d\n", errno);
				free(line_buffer);
				line_buffer = NULL;
				line_index = 0;
				bufsz = 256;
				return ECORE_CALLBACK_CANCEL;
			}

			line_buffer = new_buf;
		}

		line_buffer[line_index] = '\0';

		if (!strcmp(line_buffer, "EOD")) {
			do_line_command();
			s_info.cmd = NOP;
		} else {
			processing_line_buffer(line_buffer);
		}

		free(line_buffer);
		line_buffer = NULL;
		line_index = 0;
		bufsz = 256;
	} else {
		char *new_buf;

		line_buffer[line_index++] = ch;
		if (line_index == bufsz - 1) {
			bufsz += 256;
			new_buf = realloc(line_buffer, bufsz);
			if (!new_buf) {
				printf("realloc: %d\n", errno);
				free(line_buffer);
				line_buffer = NULL;
				line_index = 0;
				bufsz = 256;
				return ECORE_CALLBACK_CANCEL;
			}

			line_buffer = new_buf;
		}
	}

	return ECORE_CALLBACK_RENEW;
}

static int ret_cb(pid_t pid, int handle, const struct packet *packet, void *data)
{
	const char *fifo_name;
	int ret;

	if (packet_get(packet, "si", &fifo_name, &ret) != 2) {
		printf("Invalid packet\n");
		return -EFAULT;
	}

	if (ret != 0) {
		printf("Returns %d\n", ret);
		return ret;
	}

	printf("FIFO: %s\n", fifo_name);

	s_info.fifo_handle = open(fifo_name, O_RDONLY | O_NONBLOCK);
	if (s_info.fifo_handle < 0) {
		printf("open: %d\n", errno);
		s_info.fifo_handle = -EINVAL;
		ecore_main_loop_quit();
		return -EINVAL;
	}

	s_info.fd_handler = ecore_main_fd_handler_add(s_info.fifo_handle, ECORE_FD_READ, read_cb, NULL, NULL, NULL);
	if (!s_info.fd_handler) {
		printf("Failed to add a fd handler\n");
		if (close(s_info.fifo_handle) < 0) {
			printf("close: %d\n", errno);
		}
		s_info.fifo_handle = -EINVAL;
		ecore_main_loop_quit();
		return -EFAULT;
	}

	prompt(NULL);

	if (s_info.input_fd == STDIN_FILENO) {
		if (fcntl(s_info.input_fd, F_SETFL, O_NONBLOCK) < 0) {
			printf("fcntl: %d\n", errno);
		}

		s_info.in_handler = ecore_main_fd_handler_add(s_info.input_fd, ECORE_FD_READ, input_cb, NULL, NULL, NULL);
		if (!s_info.in_handler) {
			printf("Failed to add a input handler\n");
			ecore_main_loop_quit();
			return -EFAULT;
		}
	}

	return 0;
}

static int disconnected_cb(int handle, void *data)
{
	if (s_info.fd == handle) {
		printf("Disconnected\n");
		ecore_main_loop_quit();
	} else {
		printf("Disconnected: %d <> %d\n", s_info.fd, handle);
	}
	return 0;
}

static int connected_cb(int handle, void *data)
{
	struct packet *packet;

	if (s_info.fd != -EINVAL && s_info.fd != handle) {
		printf("Connected: %d <> %d\n", s_info.fd, handle);
		return 0;
	}

	printf("Connected\n");

	packet = packet_create("widget_mgr_hello", "d", 0.0f);
	if (!packet) {
		printf("Failed to build a packet for hello\n");
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
		return -EFAULT;
	}

	s_info.fd = handle;

	if (com_core_packet_async_send(s_info.fd, packet, 0.0f, ret_cb, NULL) < 0) {
		printf("Failed to send a packet hello\n");
		packet_destroy(packet);
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
		return -EFAULT;
	}

	packet_destroy(packet);
	return 0;
}

int main(int argc, char *argv[])
{
	struct termios ttystate;
	static struct method s_table[] = {
		{
			.cmd = NULL,
			.handler = NULL,
		},
	};
	static struct option long_options[] = {
		{ "batchmode", required_argument, 0, 'b' },
		{ "help", no_argument, 0, 'h' },
		{ "verbose", required_argument, 0, 'v' },
		{ "execute", required_argument, 0, 'x' },
		{ 0, 0, 0, 0 }
	};
	int option_index;
	int c;

	do {
		c = getopt_long(argc, argv, "b:hv:x:d", long_options, &option_index);
		switch (c) {
		case 'b':
			if (!optarg || !*optarg) {
				printf("Invalid argument\n");
				help();
				return -EINVAL;
			}

			if (s_info.input_fd != STDIN_FILENO) {
				/* Close the previously, opened file */
				if (close(s_info.input_fd) < 0) {
					printf("close: %d\n", errno);
				}
			}

			s_info.input_fd = open(optarg, O_RDONLY);
			if (s_info.input_fd < 0) {
				printf("Unable to access %s (%d)\n", optarg, errno);
				return -EIO;
			}
			break;
		case 'h':
			help();
			return 0;
		case 'v':
			if (!optarg || !*optarg) {
				printf("Invalid argument\n");
				help();
				return -EINVAL;
			}

			s_info.verbose = !strcmp(optarg, "true");
			break;
		case 'x':
			if (!optarg || !*optarg) {
				printf("Invalid argument\n");
				help();
				return -EINVAL;
			}
			break;
		case 'd':
			/** Dump all buffer to a files */
			s_info.dump = 1;
			break;
		default:
			break;
		}
	} while (c != -1);

	ecore_init();

#if (GLIB_MAJOR_VERSION <= 2 && GLIB_MINOR_VERSION < 36)
	g_type_init();
#endif

	/**
	 * Make the system daemon can access proc file to validate myself
	 */

	com_core_add_event_callback(CONNECTOR_DISCONNECTED, disconnected_cb, NULL);
	com_core_add_event_callback(CONNECTOR_CONNECTED, connected_cb, NULL);
	widget_service_init();

	s_info.fd = com_core_packet_client_init(SOCKET_FILE, 0, s_table);
	if (s_info.fd < 0) {
		printf("Failed to make a connection\n");
		return -EIO;
	}

	if (s_info.input_fd == STDIN_FILENO) {
		printf("Type your command on below empty line\n");

		if (tcgetattr(s_info.input_fd, &ttystate) < 0) {
			printf("Error: %d\n", errno);
		} else {
			ttystate.c_lflag &= ~(ICANON | ECHO);
			ttystate.c_cc[VMIN] = 1;

			if (tcsetattr(s_info.input_fd, TCSANOW, &ttystate) < 0) {
				printf("Error: %d\n", errno);
			}
		}
	} else {
		printf("Batch mode enabled\n");
	}

	if (setvbuf(stdout, (char *)NULL, _IONBF, 0) != 0) {
		printf("setvbuf: %d\n", errno);
	}

	init_directory();

	ecore_main_loop_begin();

	fini_directory();
	widget_service_fini();

	if (s_info.fd > 0) {
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
	}

	if (s_info.fd_handler) {
		ecore_main_fd_handler_del(s_info.fd_handler);
		s_info.fd_handler = NULL;
	}

	if (s_info.input_fd == STDIN_FILENO) {
		ttystate.c_lflag |= ICANON | ECHO;
		if (tcsetattr(s_info.input_fd, TCSANOW, &ttystate) < 0) {
			printf("tcsetattr: %d\n", errno);
		}
	} else {
		if (close(s_info.input_fd) < 0) {
			printf("close: %d\n", errno);
		}
	}

	if (s_info.fifo_handle > 0) {
		if (close(s_info.fifo_handle) < 0) {
			printf("close: %d\n", errno);
		}
		s_info.fifo_handle = -EINVAL;
	}

	if (s_info.in_handler) {
		ecore_main_fd_handler_del(s_info.in_handler);
		s_info.in_handler = NULL;
	}

	ecore_shutdown();
	putc((int)'\n', stdout);
	return 0;
}

/* End of a file */
