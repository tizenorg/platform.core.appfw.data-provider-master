#include <stdio.h>
#include <unistd.h>

#include <gio/gio.h>
#include <packet.h>
#include <com-core.h>
#include <dlog.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "fault_manager.h"
#include "util.h"
#include "debug.h"

static int evt_cb(int handle, void *data)
{
	struct slave_node *slave;
	struct client_node *client;

	slave = slave_rpc_find_by_handle(handle);
	if (slave) {
		DbgPrint("Slave is disconnected\n");
		if (slave_pid(slave) != (pid_t)-1) {
			if (slave_state(slave) == SLAVE_REQUEST_TO_TERMINATE)
				slave_deactivated(slave);
			else
				slave_deactivated_by_fault(slave);
		}

		return 0;
	}

	client = client_rpc_find_by_handle(handle);
	if (client) {
		DbgPrint("Client is disconnected\n");
		if (client_pid(client) != (pid_t)-1)
			client_deactivated_by_fault(client);

		return 0;
	}

	DbgPrint("This is not my favor: %d\n", handle);
	return 0;
}

int dead_init(void)
{
	com_core_add_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);
//	aul_listen_app_dead_signal(dead_cb, NULL);
	return 0;
}

int dead_fini(void)
{
	com_core_del_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);
	return 0;
}

/* End of a file */
