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
#include <errno.h>
#include <stdlib.h>

#include <Eina.h>
#include <dlog.h>

#include <widget_errno.h>
#include <widget_cmd_list.h>
#include <packet.h>
#include <com-core_packet.h>

#include "debug.h"
#include "dead_monitor.h"
#include "monitor.h"
#include "util.h"

int errno;

struct monitor_client {
	char *widget_id;
	pid_t pid;
	int handle;
};

static struct info {
	Eina_List *monitor_list;
} s_info = {
	.monitor_list = NULL,
};

static void monitor_disconnected_cb(int handle, void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct monitor_client *monitor;

	EINA_LIST_FOREACH_SAFE(s_info.monitor_list, l, n, monitor) {
		if (monitor->handle == handle) {
			DbgPrint("monitor: %d is deleted (%s)\n", monitor->handle, monitor->widget_id);
			s_info.monitor_list = eina_list_remove(s_info.monitor_list, monitor);
			DbgFree(monitor->widget_id);
			DbgFree(monitor);
		}
	}
}

HAPI struct monitor_client *monitor_create_client(const char *widget_id, pid_t pid, int handle)
{
	struct monitor_client *monitor;

	monitor = calloc(1, sizeof(*monitor));
	if (!monitor) {
		ErrPrint("calloc: %d\n", errno);
		return NULL;
	}

	if (widget_id) {
		monitor->widget_id = strdup(widget_id);
		if (!monitor->widget_id) {
			ErrPrint("strdup: %s, %d\n", widget_id, errno);
			DbgFree(monitor);
			return NULL;
		}
	}
	monitor->pid = pid;
	monitor->handle = handle;

	s_info.monitor_list = eina_list_append(s_info.monitor_list, monitor);

	if (dead_callback_add(handle, monitor_disconnected_cb, NULL) < 0) {
		ErrPrint("It's Okay. Dead callback is already registered for %d\n", handle);
	}

	return monitor;
}

HAPI int monitor_destroy_client(struct monitor_client *monitor)
{
	Eina_List *l;
	struct monitor_client *item;
	int handle;
	int cnt;

	s_info.monitor_list = eina_list_remove(s_info.monitor_list, monitor);
	handle = monitor->handle;
	DbgFree(monitor->widget_id);
	DbgFree(monitor);

	cnt = 0;
	EINA_LIST_FOREACH(s_info.monitor_list, l, item) {
		if (item->handle == handle) {
			cnt++;
		}
	}

	if (cnt == 0) {
		dead_callback_del(handle, monitor_disconnected_cb, NULL);
		ErrPrint("Registered monitor object is not valid\n");
	}

	return WIDGET_ERROR_NONE;
}

HAPI struct monitor_client *monitor_find_client_by_pid(const char *widget_id, pid_t pid)
{
	Eina_List *l;
	struct monitor_client *monitor;

	monitor = NULL;
	EINA_LIST_FOREACH(s_info.monitor_list, l, monitor) {
		if (monitor->pid == pid) {
			if (monitor->widget_id && widget_id) {
				if (!strcmp(monitor->widget_id, widget_id)) {
					break;
				}
			} else if (monitor->widget_id == widget_id) {
				break;
			}
		}

		monitor = NULL;
	}

	return monitor;
}

HAPI struct monitor_client *monitor_find_client_by_handle(const char *widget_id, int handle)
{
	Eina_List *l;
	struct monitor_client *monitor;

	monitor = NULL;
	EINA_LIST_FOREACH(s_info.monitor_list, l, monitor) {
		if (monitor->handle == handle) {
			if (monitor->widget_id && widget_id) {
				if (!strcmp(monitor->widget_id, widget_id)) {
					break;
				}
			} else if (monitor->widget_id == widget_id) {
				break;
			}
		}

		monitor = NULL;
	}

	return monitor;
}

HAPI int monitor_multicast_state_change_event(const char *widget_id, enum monitor_event_type event, const char *instance_id, const char *content_info)
{
	struct monitor_client *monitor;
	Eina_List *l;
	int cnt;
	unsigned int cmd;
	struct packet *packet;

	cmd = (unsigned int)event;

	packet = packet_create_noack((const char *)&cmd, "dsss", util_timestamp(), widget_id, instance_id, content_info);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return WIDGET_ERROR_FAULT;
	}

	cnt = 0;
	EINA_LIST_FOREACH(s_info.monitor_list, l, monitor) {
		if (widget_id && monitor->widget_id) {
			if (!strcmp(widget_id, monitor->widget_id)) {
				com_core_packet_send_only(monitor->handle, packet);
				cnt++;
			}
		} else if (monitor->widget_id == NULL) {
			com_core_packet_send_only(monitor->handle, packet);
			cnt++;
		}
	}

	packet_destroy(packet);

	DbgPrint("%d events are multicasted\n", cnt);
	return cnt;
}

/* End of a file */
