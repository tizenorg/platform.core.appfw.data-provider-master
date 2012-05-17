#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>

#include <gio/gio.h>

#include <aul.h>
#include <dlog.h>

#include "slave_manager.h"
#include "client_manager.h"
#include "fault_manager.h"
#include "debug.h"

int aul_listen_app_dead_signal(int (*)(int, void *), void *);

static int dead_cb(int pid, void *cb_data)
{
	struct slave_node *slave;
	struct client_node *client;

	slave = slave_find_by_pid(pid);
	if (slave) {
		slave_fault_deactivating(slave, 0);
		return 0;
	}

	client = client_find(pid);
	if (client) {
		client_fault_deactivating(client);
		return 0;
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
