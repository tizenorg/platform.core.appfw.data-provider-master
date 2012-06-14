#include <stdio.h>
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>

#include "debug.h"
#include "packet.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "package.h"
#include "fault_manager.h"
#include "util.h"
#include "conf.h"
#include "connector_packet.h"

struct slave_rpc {
	Ecore_Timer *pong_timer;
	int handle;

	unsigned long ping_count;
	unsigned long next_ping_count;
	Eina_List *pending_list;

	struct slave_node *slave;
};

struct command {
	/* create_command, destroy_command will care these varaibles */
	char *pkgname;
	struct packet *packet;
	struct slave_node *slave;

	/* Don't need to care these data */
	void (*ret_cb)(struct slave_node *slave, const struct packet *packet, void *cbdata);
	void *cbdata;
};

static struct info {
	Eina_List *command_list;
	Eina_List *rpc_list;
	Ecore_Timer *command_consuming_timer;
} s_info = {
	.command_list = NULL,
	.command_consuming_timer = NULL,
	.rpc_list = NULL,
};

static inline struct command *create_command(struct slave_node *slave, const char *pkgname, struct packet *packet)
{
	struct command *command;

	command = calloc(1, sizeof(*command));
	if (!command) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	if (pkgname) {
		command->pkgname = strdup(pkgname);
		if (!command->pkgname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(command);
			return NULL;
		}
	}

	command->slave = slave_ref(slave); /*!< To prevent from destroying of the slave while communicating with the slave */
	command->packet = packet_ref(packet);

	return command;
}

static inline void destroy_command(struct command *command)
{
	slave_unref(command->slave);
	packet_unref(command->packet);
	free(command->pkgname);
	free(command);
}

static inline struct command *pop_command(void)
{
	struct command *command;

	command = eina_list_nth(s_info.command_list, 0);
	if (!command)
		return NULL;

	s_info.command_list = eina_list_remove(s_info.command_list, command);
	return command;
}

static int slave_async_cb(pid_t pid, int handle, const struct packet *packet, void *data)
{
	struct command *command = data;

	if (!command) {
		ErrPrint("Packet is NIL\n");
		return 0;
	}

	/*!
	 * \note
	 * command->packet is not valid from here.
	 */
	if (!slave_is_activated(command->slave)) {
		ErrPrint("Slave is not activated (accidently dead)\n");
		if (command->ret_cb)
			command->ret_cb(command->slave, packet, command->cbdata);

		goto out;
	}

	if (!packet) {
		if (command->ret_cb)
			command->ret_cb(command->slave, packet, command->cbdata);

		slave_faulted(command->slave);
		goto out;
	}

	if (command->ret_cb)
		command->ret_cb(command->slave, packet, command->cbdata);

out:
	destroy_command(command);
	return 0;
}

static Eina_Bool command_consumer_cb(void *data)
{
	struct command *command;
	struct slave_rpc *rpc;

	command = pop_command();
	if (!command) {
		s_info.command_consuming_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	if (!slave_is_activated(command->slave)) {
		ErrPrint("Slave is not activated: %s(%d)\n",
				slave_name(command->slave), slave_pid(command->slave));
		goto errout;
	}

	if (!command->pkgname) {
		/*!
		 * \note
		 * pause or resume command has no package name
		 */
		DbgPrint("Package name is not specified: command(%s)\n",
						packet_command(command->packet));
	} else {
		struct pkg_info *info;

		info = package_find(command->pkgname);
		if (info && package_is_fault(info)) {
			ErrPrint("info: %p (%s) is fault package\n", info, command->pkgname);
			goto errout;
		}
	}

	rpc = slave_data(command->slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave has no rpc info\n");
		goto errout;
	}

	if (connector_packet_async_send(rpc->handle, command->packet, slave_async_cb, command) == 0)
		return ECORE_CALLBACK_RENEW;

errout:
	if (command->ret_cb)
		command->ret_cb(command->slave, NULL, command->cbdata);
	destroy_command(command);
	return ECORE_CALLBACK_RENEW;
}

static inline void push_command(struct command *command)
{
	s_info.command_list = eina_list_append(s_info.command_list, command);

	if (s_info.command_consuming_timer)
		return;

	s_info.command_consuming_timer = ecore_timer_add(PACKET_TIME, command_consumer_cb, NULL);
	if (!s_info.command_consuming_timer) {
		ErrPrint("Failed to add command consumer\n");
		s_info.command_list = eina_list_remove(s_info.command_list, command);
		destroy_command(command);
	}
}

static int slave_deactivate_cb(struct slave_node *slave, void *data)
{
	struct slave_rpc *rpc;
	struct command *command;
	Eina_List *l;
	Eina_List *n;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		/*!
		 * \note
		 * Return negative value will remove this callback from the event list of the slave
		 */
		return -EINVAL;
	}

	if (rpc->pong_timer)
		ecore_timer_del(rpc->pong_timer);
	else
		ErrPrint("slave has no pong timer\n");

	EINA_LIST_FOREACH_SAFE(s_info.command_list, l, n, command) {
		if (command->slave == slave) {
			s_info.command_list = eina_list_remove(s_info.command_list, command);
			destroy_command(command);
		}
	}

	/*!
	 * \todo
	 * Make statistics table
	 */
	rpc->ping_count = 0;
	rpc->next_ping_count = 1;
	return 0;
}

static int slave_del_cb(struct slave_node *slave, void *data)
{
	struct slave_rpc *rpc;

	slave_event_callback_del(slave, SLAVE_EVENT_DELETE, slave_del_cb, NULL);
	slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb, NULL);

	rpc = slave_del_data(slave, "rpc");
	if (!rpc)
		return 0;

	if (rpc->pong_timer)
		ecore_timer_del(rpc->pong_timer);
	else
		ErrPrint("Pong timer is not exists\n");

	s_info.rpc_list = eina_list_remove(s_info.rpc_list, rpc);
	free(rpc);
	return 0;
}

static Eina_Bool ping_timeout_cb(void *data)
{
	struct slave_rpc *rpc;
	struct slave_node *slave = data;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave RPC is not valid\n");
		return ECORE_CALLBACK_CANCEL;
	}

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is not activated\n");
		return ECORE_CALLBACK_CANCEL;
	}

	/*!
	 * Dead callback will handling this
	 */
	slave_faulted(slave);
	return ECORE_CALLBACK_CANCEL;
}

static inline int slave_rpc_is_valid(struct slave_rpc *rpc)
{
	return rpc && rpc->handle > 0;
}

int slave_rpc_async_request(struct slave_node *slave, const char *pkgname, struct packet *packet, void (*ret_cb)(struct slave_node *slave, const struct packet *packet, void *data), void *data)
{
	struct command *command;
	struct slave_rpc *rpc;

	command = create_command(slave, pkgname, packet);
	if (!command) {
		if (ret_cb)
			ret_cb(slave, NULL, data);

		packet_unref(packet);
		return -ENOMEM;
	}

	command->ret_cb = ret_cb;
	command->cbdata = data;

	rpc = slave_data(slave, "rpc");
	if (!slave_rpc_is_valid(rpc)) {
		DbgPrint("RPC info is not ready to use, push this to pending list\n");
		rpc->pending_list = eina_list_append(rpc->pending_list, command);
		return 0;
	}

	push_command(command);
	return 0;
}

int slave_rpc_update_handle(struct slave_node *slave, int handle)
{
	struct slave_rpc *rpc;
	struct command *command;

	rpc = slave_data(slave, "rpc");
	if (!rpc)
		return -EINVAL;

	rpc->handle = handle;
	if (rpc->pong_timer)
		ecore_timer_del(rpc->pong_timer);

	rpc->pong_timer = ecore_timer_add(g_conf.ping_time, ping_timeout_cb, slave);
	if (!rpc->pong_timer)
		ErrPrint("Failed to add ping timer\n");

	slave_activated(slave);
	slave_reset_fault(slave);

	EINA_LIST_FREE(rpc->pending_list, command) {
		push_command(command);
	}

	return 0;
}

int slave_rpc_initialize(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = calloc(1, sizeof(*rpc));
	if (!rpc) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (slave_set_data(slave, "rpc", rpc) < 0) {
		free(rpc);
		return -ENOMEM;
	}

	slave_event_callback_add(slave, SLAVE_EVENT_DELETE, slave_del_cb, NULL);
	slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb, NULL);

	rpc->ping_count = 0;
	rpc->next_ping_count = 1;
	rpc->handle = -1;
	rpc->slave = slave;

	s_info.rpc_list = eina_list_append(s_info.rpc_list, rpc);
	return 0;
}

int slave_rpc_ping(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave RPC is not valid\n");
		return -EINVAL;
	}

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is not activated\n");
		return -EFAULT;
	}

	rpc->ping_count++;
	if (rpc->ping_count != rpc->next_ping_count) {
		ErrPrint("Ping count is not correct\n");
		rpc->next_ping_count = rpc->ping_count;
	}
	rpc->next_ping_count++;

	ecore_timer_reset(rpc->pong_timer);
	return 0;
}

void slave_rpc_request_update(const char *pkgname, const char *cluster, const char *category)
{
	struct slave_node *slave;
	struct pkg_info *info;
	struct packet *packet;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("Failed to find a package\n");
		return;
	}

	slave = package_slave(info);
	if (!slave) {
		ErrPrint("Failed to find a slave for %s\n", pkgname);
		return;
	}

	packet = packet_create("update_content", "sss", pkgname, cluster, category);
	if (!packet) {
		ErrPrint("Failed to create a new param\n");
		return;
	}

	(void)slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
}

struct slave_node *slave_rpc_find_by_handle(int handle)
{
	Eina_List *l;
	struct slave_rpc *rpc;

	EINA_LIST_FOREACH(s_info.rpc_list, l, rpc) {
		if (rpc->handle == handle)
			return rpc->slave;
	}

	return NULL;
}

/* End of a file */
