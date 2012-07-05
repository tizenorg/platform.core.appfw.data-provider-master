#include <stdio.h>
#include <errno.h>

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>
#include <com-core_packet.h>
#include <packet.h>

#include "client_life.h"
#include "client_rpc.h"
#include "debug.h"
#include "conf.h"
#include "util.h"

/*!
 * \note
 * Static component information structure.
 */
static struct info {
	Eina_List *command_list; /*!< Packet Q: Before sending the request, all request commands will stay here */
	Ecore_Timer *command_consumer; /*!< This timer will consuming the command Q. sending them to the specified client */
	Eina_List *rpc_list; /*!< Create RPC object list, to find a client using given RPC info (such as GDBusConnection*, GDBusProxy*) */
} s_info = {
	.command_list = NULL,
	.command_consumer = NULL,
	.rpc_list = NULL,
};

struct client_rpc {
	int handle; /*!< Handler for communication with client */
	struct client_node *client; /*!< client_life object */
};

struct command {
	struct packet *packet;
	struct client_node *client; /*!< Target client. who should receive this command */
};

/*!
 * \brief
 * Creating or Destroying command object
 */
static inline struct command *create_command(struct client_node *client, struct packet *packet)
{
	struct command *command;

	command = calloc(1, sizeof(*command));
	if (!command) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	command->packet = packet_ref(packet);
	command->client = client_ref(client);

	return command;
}

static inline void destroy_command(struct command *command)
{
	client_unref(command->client);
	packet_unref(command->packet);
	free(command);
}

static inline int count_command(void)
{
	return eina_list_count(s_info.command_list);
}

static int recv_cb(pid_t pid, int handle, const struct packet *packet, void *data)
{
	struct command *command = data;

	if (!packet) {
		DbgPrint("Client fault?\n");
		client_fault(command->client);
	} else {
		int ret;

		packet_get(packet, "i", &ret);
		DbgPrint("returns %d\n", ret);
	}

	destroy_command(command);
	return 0;
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

static Eina_Bool command_consumer_cb(void *data)
{
	struct command *command;
	struct client_rpc *rpc;

	command = pop_command();
	if (!command) {
		s_info.command_consumer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	if (!client_is_activated(command->client)) {
		ErrPrint("Client is not activated, destroy this command\n");
		destroy_command(command);
		return ECORE_CALLBACK_RENEW;
	}

	if (client_is_faulted(command->client)) {
		ErrPrint("Client is faulted, discard command\n");
		destroy_command(command);
		return ECORE_CALLBACK_RENEW;
	}

	rpc = client_data(command->client, "rpc");
	if (!rpc) {
		ErrPrint("Invalid command\n");
		destroy_command(command);
		return ECORE_CALLBACK_RENEW;
	}

	if (rpc->handle < 0) {
		destroy_command(command);
		return ECORE_CALLBACK_RENEW;
	}

	DbgPrint("Send a packet to client [%s]\n", packet_command(command->packet));
	if (com_core_packet_async_send(rpc->handle, command->packet, recv_cb, command) < 0)
		destroy_command(command);

	return ECORE_CALLBACK_RENEW;
}

static inline void push_command(struct command *command)
{
	s_info.command_list = eina_list_append(s_info.command_list, command);

	if (s_info.command_consumer)
		return;

	s_info.command_consumer = ecore_timer_add(PACKET_TIME, command_consumer_cb, NULL);
	if (!s_info.command_consumer) {
		ErrPrint("Failed to add command consumer\n");
		s_info.command_list = eina_list_remove(s_info.command_list, command);
		destroy_command(command);
	}
}

int client_rpc_async_request(struct client_node *client, struct packet *packet)
{
	struct command *command;
	struct client_rpc *rpc;

	if (client_is_faulted(client)) {
		ErrPrint("Client is faulted\n");
		packet_unref(packet);
		return -EFAULT;
	}

	rpc = client_data(client, "rpc");
	if (!rpc)
		ErrPrint("Client is not ready for communication (%s)\n",
							packet_command(packet));

	command = create_command(client, packet);
	if (!command) {
		packet_unref(packet);
		return -ENOMEM;
	}

	push_command(command);
	packet_unref(packet);
	return 0;
}

int client_rpc_broadcast(struct packet *packet)
{
	Eina_List *l;
	struct client_rpc *rpc;

	EINA_LIST_FOREACH(s_info.rpc_list, l, rpc) {
		(void)client_rpc_async_request(rpc->client, packet_ref(packet));
	}

	packet_unref(packet);
	return 0;
}

static int deactivated_cb(struct client_node *client, void *data)
{
	struct client_rpc *rpc;
	struct command *command;
	Eina_List *l;
	Eina_List *n;

	rpc = client_data(client, "rpc");
	if (!rpc) {
		ErrPrint("client is not valid\n");
		return 0;
	}

	DbgPrint("Reset handle for %d\n", client_pid(client));
	rpc->handle = -1;

	EINA_LIST_FOREACH_SAFE(s_info.command_list, l, n, command) {
		if (command->client == client)
			destroy_command(command);
	}

	return 0;
}

static int del_cb(struct client_node *client, void *data)
{
	struct client_rpc *rpc;

	rpc = client_del_data(client, "rpc");
	if (!rpc) {
		ErrPrint("RPC is not valid\n");
		return -EINVAL;
	}

	s_info.rpc_list = eina_list_remove(s_info.rpc_list, rpc);
	free(rpc);

	client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, deactivated_cb, NULL);
	return -1; /* Return <0, Delete this callback */
}

int client_rpc_initialize(struct client_node *client, int handle)
{
	struct client_rpc *rpc;
	int ret;

	rpc = calloc(1, sizeof(*rpc));
	if (!rpc) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	ret = client_set_data(client, "rpc", rpc);
	if (ret < 0) {
		ErrPrint("Failed to set \"rpc\" for client\n");
		free(rpc);
		return ret;
	}

	DbgPrint("CLIENT: New handle assigned for %d, %d\n", client_pid(client), handle);
	rpc->handle = handle;

	client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, deactivated_cb, NULL);
	client_event_callback_add(client, CLIENT_EVENT_DESTROY, del_cb, NULL);
	rpc->client = client;

	s_info.rpc_list = eina_list_append(s_info.rpc_list, rpc);
	return 0;
}

struct client_node *client_rpc_find_by_handle(int handle)
{
	Eina_List *l;
	struct client_rpc *rpc;

	EINA_LIST_FOREACH(s_info.rpc_list, l, rpc) {
		if (rpc->handle != handle)
			continue;

		return rpc->client;
	}

	return NULL;
}

/* End of a file */
