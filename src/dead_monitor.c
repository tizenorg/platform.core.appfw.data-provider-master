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

#if 0
int aul_listen_app_dead_signal(int (*)(int, void *), void *);

static int dead_cb(int pid, void *cb_data)
{
	struct slave_node *slave;

	slave = slave_find_by_pid(pid);
	if (slave) {
		slave_deactivated_by_fault(slave);
	} else {
		struct client_node *client;
		client = client_find_by_pid(pid);
		if (client) {
			DbgPrint("Client %d is deactivated\n", client_pid(client));
			client_deactivated_by_fault(client);
		} else {
			ErrPrint("Unknown PID:%d is terminated\n", pid);
			/*!
			 * \note
			 * Ignore this dead signal
			 */
		}
	}

	return 0;
}
#endif
static int evt_cb(int handle, void *data)
{
	struct slave_node *slave;
	struct client_node *client;

	slave = slave_rpc_find_by_handle(handle);
	if (slave) {
		DbgPrint("Slave is disconnected\n");
		if (slave_pid(slave) != (pid_t)-1)
			slave_deactivated_by_fault(slave);
		return 0;
	}

	client = client_rpc_find_by_handle(handle);
	if (client) {
		DbgPrint("Client is disconnected\n");
		if (client_pid(client) != (pid_t)-1)
			client_fault(client);
		return 0;
	}

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
