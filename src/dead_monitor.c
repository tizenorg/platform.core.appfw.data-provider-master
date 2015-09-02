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

#include <gio/gio.h>
#include <packet.h>
#include <com-core.h>
#include <dlog.h>

#include <Eina.h>
#include <widget_errno.h>
#include <widget_service.h> /* destroy_type for instance.h */
#include <widget_service_internal.h> /* destroy_type for instance.h */

#include "slave_life.h"
#include "client_life.h"
#include "instance.h"
#include "fault_manager.h"
#include "util.h"
#include "debug.h"
#include "widget-mgr.h"
#include "conf.h"

struct cb_item {
	int handle;
	int disconnected;
	void (*dead_cb)(int handle, void *data);
	void *data;
};

static struct info {
	Eina_List *cb_list;
} s_info = {
	.cb_list = NULL,
};

static int evt_cb(int handle, void *data)
{
	struct slave_node *slave;
	struct client_node *client;
	struct widget_mgr *widget_mgr;
	struct cb_item *dead_item;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(s_info.cb_list, l, n, dead_item) {
		/**
		 * If the callback is called already,
		 * Do not call it again.
		 * Using "disconnected" varaible, prevent from duplicated callback call.
		 */
		if (dead_item->handle == handle) {
			if (dead_item->disconnected == 0) {
				dead_item->dead_cb(dead_item->handle, dead_item->data);
				dead_item->disconnected = 1;
				break;
			}
		}
	}

	slave = slave_find_by_rpc_handle(handle);
	if (slave) {
		if (slave_pid(slave) != (pid_t)-1) {
			switch (slave_state(slave)) {
			case SLAVE_REQUEST_TO_DISCONNECT:
				DbgPrint("Disconnected from %d\n", slave_pid(slave));
			case SLAVE_REQUEST_TO_TERMINATE:
				slave = slave_deactivated(slave);
				break;
			default:
				if (slave_wait_deactivation(slave)) {
					/**
					 * @note
					 * Slave is waiting the termination,
					 * in this case, it should be dealt as a normal termination.
					 */

					DbgPrint("Slave is waiting deactivation, Do not re-activate automatically in this case\n");
					slave_set_wait_deactivation(slave, 0);
					slave_set_reactivation(slave, 0);
					slave_set_reactivate_instances(slave, 1);

					slave = slave_deactivated(slave);
				} else {
					slave = slave_deactivated_by_fault(slave);
				}
				break;
			}
		}

		if (!slave) {
			DbgPrint("Slave is deleted\n");
		}

		return 0;
	}

	client = client_find_by_rpc_handle(handle);
	if (client) {
		if (client_pid(client) != (pid_t)-1) {
			client = client_deactivated_by_fault(client);
		}

		if (!client) {
			DbgPrint("Client is deleted\n");
		}

		return 0;
	}

	widget_mgr = widget_mgr_find_by_handle(handle);
	if (widget_mgr) {
		widget_mgr_destroy(widget_mgr);
		return 0;
	}

	return 0;
}

HAPI int dead_init(void)
{
	com_core_add_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);
	return 0;
}

HAPI int dead_fini(void)
{
	struct cb_item *item;

	com_core_del_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);

	EINA_LIST_FREE(s_info.cb_list, item) {
		DbgFree(item);
	}

	return 0;
}

HAPI int dead_callback_add(int handle, void (*dead_cb)(int handle, void *data), void *data)
{
	struct cb_item *item;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.cb_list, l, item) {
		if (item->handle == handle && item->disconnected == 0) {
			return WIDGET_ERROR_ALREADY_EXIST;
		}
	}

	item = malloc(sizeof(*item));
	if (!item) {
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	item->handle = handle;
	item->dead_cb = dead_cb;
	item->data = data;
	/**
	 * Handle can be reallocated if the first connection is disconnected,
	 * The kernel can resue the last handle (index) for newly comming connection.
	 * So we have to check the callback data using disconnected field.
	 * If the connection is disconnected first, we have to toggle this to true.
	 */
	item->disconnected = 0;

	s_info.cb_list = eina_list_append(s_info.cb_list, item);
	return 0;
}

HAPI void *dead_callback_del(int handle, void (*dead_cb)(int handle, void *data), void *data)
{
	struct cb_item *item;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(s_info.cb_list, l, n, item) {
		if (item->handle == handle && item->dead_cb == dead_cb && item->data == data) {
			void *cbdata;

			s_info.cb_list = eina_list_remove(s_info.cb_list, item);
			cbdata = item->data;
			DbgFree(item);

			return cbdata;
		}
	}

	return NULL;
}

/* End of a file */
