#include <stdio.h>
#include <unistd.h>
#include <libgen.h>

#include <aul.h>
#include <dlog.h>

#include <gio/gio.h>

#include "slave_life.h"
#include "client_manager.h"
#include "fault_manager.h"
#include "debug.h"

int aul_listen_app_dead_signal(int (*)(int, void *), void *);

static int dead_cb(int pid, void *cb_data)
{
	struct slave_node *slave;

	slave = slave_find_by_pid(pid);
	if (slave) {
		DbgPrint("Slave %p is dead\n");
		slave_deactivated_by_fault(slave);
	} else {
		struct client_node *client;
		client = client_find(pid);
		if (client) {
			client_destroy(client);
		} else {
			/* Ignore this dead signal */
		}
	}

	return 0;
}

int dead_init(void)
{
	aul_listen_app_dead_signal(dead_cb, NULL);
	return 0;
}

int dead_fini(void)
{
	return 0;
}

/* End of a file */
