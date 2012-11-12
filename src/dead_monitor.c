#include <stdio.h>
#include <unistd.h>

#include <gio/gio.h>
#include <packet.h>
#include <com-core.h>
#include <dlog.h>

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
		DbgPrint("Slave is disconnected %d\n", handle);
		if (slave_pid(slave) != (pid_t)-1) {
			if (slave_state(slave) == SLAVE_REQUEST_TO_TERMINATE)
				slave = slave_deactivated(slave);
			else if (slave_state(slave) != SLAVE_TERMINATED)
				slave = slave_deactivated_by_fault(slave);
		}

		DbgPrint("Slave pointer: %p (0x0 means deleted)\n", slave);
		return 0;
	}

	client = client_find_by_rpc_handle(handle);
	if (client) {
		DbgPrint("Client is disconnected\n");
		if (client_pid(client) != (pid_t)-1)
			client_deactivated_by_fault(client);

		return 0;
	}

	liveinfo = liveinfo_find_by_handle(handle);
	if (liveinfo) {
		DbgPrint("Utility is disconnected\n");
		liveinfo_destroy(liveinfo);
		return 0;
	}

	DbgPrint("This is not my favor: %d\n", handle);
	return 0;
}

HAPI int dead_init(void)
{
	com_core_add_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);
//	aul_listen_app_dead_signal(dead_cb, NULL);
	return 0;
}

HAPI int dead_fini(void)
{
	com_core_del_event_callback(CONNECTOR_DISCONNECTED, evt_cb, NULL);
	return 0;
}

/* End of a file */
