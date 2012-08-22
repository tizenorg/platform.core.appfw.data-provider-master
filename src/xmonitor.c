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

#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <assert.h>
#include <errno.h>

#include <Evas.h>
#include <Ecore_X.h>
#include <Ecore.h>

#include <gio/gio.h>
#include <dlog.h>

#include "conf.h"
#include "xmonitor.h"
#include "debug.h"
#include "client_life.h"
#include "slave_life.h"
#include "main.h"
#include "util.h"
#include "setting.h"

int errno;

static struct info {
	Ecore_Event_Handler *create_handler;
	Ecore_Event_Handler *destroy_handler;
	Ecore_Event_Handler *client_handler;
} s_info = {
	.create_handler = NULL,
	.destroy_handler = NULL,
	.client_handler = NULL,
};

static inline int get_pid(Ecore_X_Window win)
{
	int pid;
	Ecore_X_Atom atom;
	unsigned char *in_pid;
	int num;

	atom = ecore_x_atom_get("X_CLIENT_PID");
	if (ecore_x_window_prop_property_get(win, atom, ECORE_X_ATOM_CARDINAL,
				sizeof(int), &in_pid, &num) == EINA_FALSE) {
		if (ecore_x_netwm_pid_get(win, &pid) == EINA_FALSE) {
			ErrPrint("Failed to get PID from a window 0x%X\n", win);
			return -EINVAL;
		}
	} else {
		pid = *(int *)in_pid;
		free(in_pid);
	}

	return pid;
}

static Eina_Bool create_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Window_Create *info = event;
	ecore_x_window_client_sniff(info->win);
	return ECORE_CALLBACK_RENEW;
}

static Eina_Bool destroy_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Window_Destroy * info;
	info = event;
	return ECORE_CALLBACK_RENEW;
}

int xmonitor_update_state(int target_pid)
{
	Ecore_X_Window win;
	struct client_node *client;
	int pid;

	win = ecore_x_window_focus_get();

	pid = get_pid(win);
	if (pid <= 0) {
		DbgPrint("Focused window has no PID %X\n", win);
		client = client_find_by_pid(target_pid);
		if (client) {
			DbgPrint("Client window has not focus now\n");
			client_paused(client);
		}
		return -ENOENT;
	}

	client = client_find_by_pid(pid);
	if (!client) {
		DbgPrint("Client %d is not registered yet\n", pid);
		client = client_find_by_pid(target_pid);
		if (client) {
			DbgPrint("Client window has not focus now\n");
			client_paused(client);
		}
		return -EINVAL;
	}

	if (target_pid != pid) {
		DbgPrint("Client is paused\n");
		client_paused(client);
	} else {
		DbgPrint("Client is resumed\n");
		client_resumed(client);
	}

	slave_handle_state_change();
	return 0;
}

static Eina_Bool client_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Client_Message *info = event;
	struct client_node *client;
	char *name;
	int pid;

	pid = get_pid(info->win);
	if (pid <= 0)
		return ECORE_CALLBACK_RENEW;

	client = client_find_by_pid(pid);
	if (!client)
		return ECORE_CALLBACK_RENEW;

	name = ecore_x_atom_name_get(info->message_type);
	if (!name)
		return ECORE_CALLBACK_RENEW;

	if (!strcmp(name, "_X_ILLUME_DEACTIVATE_WINDOW")) {
		DbgPrint("PAUSE EVENT\n");
		client_paused(client);

		slave_handle_state_change();
	} else if (!strcmp(name, "_X_ILLUME_ACTIVATE_WINDOW")) {
		DbgPrint("RESUME EVENT\n");
		client_resumed(client);

		slave_handle_state_change();
	} else {
		/* ignore event */
	}

	free(name);
	return ECORE_CALLBACK_RENEW;
}

static inline void sniff_all_windows(void)
{
	Ecore_X_Window root;
	Ecore_X_Window ret;
	struct stack_item *new_item;
	struct stack_item *item;
	Eina_List *win_stack;
	//int pid;
	struct stack_item {
		Ecore_X_Window *wins;
		int nr_of_wins;
		int i;
	};

	root = ecore_x_window_root_first_get();
	ecore_x_window_sniff(root);

	new_item = malloc(sizeof(*new_item));
	if (!new_item) {
		ErrPrint("Error(%s)\n", strerror(errno));
		return;
	}

	new_item->nr_of_wins = 0;
	new_item->wins =
		ecore_x_window_children_get(root, &new_item->nr_of_wins);
	new_item->i = 0;

	win_stack = NULL;

	if (new_item->wins)
		win_stack = eina_list_append(win_stack, new_item);
	else
		free(new_item);

	while ((item = eina_list_nth(win_stack, 0))) {
		win_stack = eina_list_remove(win_stack, item);

		if (!item->wins) {
			free(item);
			continue;
		}

		while (item->i < item->nr_of_wins) {
			ret = item->wins[item->i];

			/*
			 * Now we don't need to care about visibility of window,
			 * just check whether it is registered or not.
			 * (ecore_x_window_visible_get(ret))
			 */
			ecore_x_window_client_sniff(ret);

			new_item = malloc(sizeof(*new_item));
			if (!new_item) {
				ErrPrint("Error %s\n", strerror(errno));
				item->i++;
				continue;
			}

			new_item->i = 0;
			new_item->nr_of_wins = 0;
			new_item->wins =
				ecore_x_window_children_get(ret,
							&new_item->nr_of_wins);
			if (new_item->wins) {
				win_stack =
					eina_list_append(win_stack, new_item);
			} else {
				free(new_item);
			}

			item->i++;
		}

		free(item->wins);
		free(item);
	}

	return;
}

int xmonitor_init(void)
{
	if (ecore_x_composite_query() == EINA_FALSE)
		DbgPrint("====> COMPOSITOR IS NOT ENABLED\n");

	s_info.create_handler =
		ecore_event_handler_add(ECORE_X_EVENT_WINDOW_CREATE,
							create_cb, NULL);
	if (!s_info.create_handler) {
		ErrPrint("Failed to add create event handler\n");
		return -EFAULT;
	}

	s_info.destroy_handler =
		ecore_event_handler_add(ECORE_X_EVENT_WINDOW_DESTROY,
							destroy_cb, NULL);
	if (!s_info.create_handler) {
		ErrPrint("Failed to add destroy event handler\n");
		ecore_event_handler_del(s_info.create_handler);
		s_info.create_handler = NULL;
		return -EFAULT;
	}

	s_info.client_handler =
		ecore_event_handler_add(ECORE_X_EVENT_CLIENT_MESSAGE,
							client_cb, NULL);
	if (!s_info.client_handler) {
		ErrPrint("Failed to add focus out event handler\n");
		ecore_event_handler_del(s_info.create_handler);
		ecore_event_handler_del(s_info.destroy_handler);
		s_info.create_handler = NULL;
		s_info.destroy_handler = NULL;
		return -EFAULT;
	}

	sniff_all_windows();
	return 0;
}

void xmonitor_fini(void)
{
	ecore_event_handler_del(s_info.create_handler);
	ecore_event_handler_del(s_info.destroy_handler);
	ecore_event_handler_del(s_info.client_handler);

	s_info.create_handler = NULL;
	s_info.destroy_handler = NULL;
	s_info.client_handler = NULL;
}

/* End of a file */
