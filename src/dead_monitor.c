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

#include "slave_life.h"
#include "client_life.h"
#include "instance.h"
#include "fault_manager.h"
#include "util.h"
#include "debug.h"
#include "liveinfo.h"
#include "conf.h"

static int evt_cb(int handle, void *data)
{
	struct slave_node *slave;
	struct client_node *client;
	struct liveinfo *liveinfo;

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
				slave = slave_deactivated_by_fault(slave);
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

	liveinfo = liveinfo_find_by_handle(handle);
	if (liveinfo) {
		liveinfo_destroy(liveinfo);
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
	com_core_del_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);
	return 0;
}

/* End of a file */
