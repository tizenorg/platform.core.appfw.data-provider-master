#include <stdio.h>
#include <libgen.h>
#include <errno.h>

#include <Eina.h>
#include <Ecore.h>

#include <gio/gio.h>
#include <dlog.h>

#include "client_life.h"
#include "client_rpc.h"
#include "debug.h"
#include "conf.h"

/*!
 * \note
 * Static component information structure.
 */
static struct info {
	Eina_List *packet_list; /*!< Packet Q: Before sending the request, all request packets will stay here */
	Ecore_Timer *packet_consumer; /*!< This timer will consuming the packet Q. sending them to the specified client */
	Eina_List *rpc_list; /*!< Create RPC object list, to find a client using given RPC info (such as GDBusConnection*, GDBusProxy*) */
} s_info = {
	.packet_list = NULL,
	.packet_consumer = NULL,
	.rpc_list = NULL,
};

struct client_rpc {
	GDBusProxy *proxy; /*!< Proxy object for this client */
	GDBusConnection *conn; /*!< Connection object for this client */
	Eina_List *pending_request_list; /*!< Before making connection, this Q will be used for keeping the request packets */
	struct client_node *client; /*!< client_life object */
};

struct packet {
	char *funcname; /*!< Method name which is supported by the livebox-viewer/dbus.c */
	GVariant *param; /*!< Parameter object */
	struct client_node *client; /*!< Target client. who should receive this packet */
};

/*!
 * \brief
 * Creating or Destroying packet object
 */
static inline struct packet *create_packet(struct client_node *client, const char *func, GVariant *param)
{
	struct packet *packet;

	packet = calloc(1, sizeof(*packet));
	if (!packet) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	packet->funcname = strdup(func);
	if (!packet->funcname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(packet);
		return NULL;
	}

	packet->param = g_variant_ref(param);
	packet->client = client_ref(client);

	return packet;
}

static inline void destroy_packet(struct packet *packet)
{
	client_unref(packet->client);
	free(packet->funcname);
	g_variant_unref(packet->param);
	free(packet);
}

static inline int count_packet(void)
{
	return eina_list_count(s_info.packet_list);
}

/*!
 */
static void client_cmd_done(GDBusProxy *proxy, GAsyncResult *res, void *item)
{
	GVariant *result;
	GError *err = NULL;
	struct packet *packet = item;

	result = g_dbus_proxy_call_finish(proxy, res, &err);
	if (result) {
		int ret;

		g_variant_get(result, "(i)", &ret);
		g_variant_unref(result);
	} else {
		if (err) {
			ErrPrint("Error: %s\n", err->message);
			g_error_free(err);
		}

		client_fault(packet->client);
	}

	destroy_packet(packet);
}

static inline struct packet *pop_packet(void)
{
	struct packet *packet;

	packet = eina_list_nth(s_info.packet_list, 0);
	if (!packet)
		return NULL;

	s_info.packet_list = eina_list_remove(s_info.packet_list, packet);
	return packet;
}

static Eina_Bool packet_consumer_cb(void *data)
{
	struct packet *packet;
	struct client_rpc *rpc;

	packet = pop_packet();
	if (!packet) {
		s_info.packet_consumer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	if (!client_is_activated(packet->client)) {
		ErrPrint("Client is not activated, destroy this packet\n");
		destroy_packet(packet);
		return ECORE_CALLBACK_RENEW;
	}

	if (client_is_faulted(packet->client)) {
		ErrPrint("Client is faulted, discard packet\n");
		destroy_packet(packet);
		return ECORE_CALLBACK_RENEW;
	}

	rpc = client_data(packet->client, "rpc");
	if (!rpc) {
		ErrPrint("Invalid packet\n");
		destroy_packet(packet);
		return ECORE_CALLBACK_RENEW;
	}

	g_dbus_proxy_call(rpc->proxy,
		packet->funcname, packet->param,
		G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL,
		(GAsyncReadyCallback)client_cmd_done, packet);

	return ECORE_CALLBACK_RENEW;
}

static inline void push_packet(struct packet *packet)
{
	s_info.packet_list = eina_list_append(s_info.packet_list, packet);

	if (s_info.packet_consumer)
		return;

	s_info.packet_consumer = ecore_timer_add(PACKET_TIME, packet_consumer_cb, NULL);
	if (!s_info.packet_consumer) {
		ErrPrint("Failed to add packet consumer\n");
		s_info.packet_list = eina_list_remove(s_info.packet_list, packet);
		destroy_packet(packet);
	}
}

int client_rpc_async_request(struct client_node *client, const char *funcname, GVariant *param)
{
	struct packet *packet;
	struct client_rpc *rpc;

	if (client_is_faulted(client)) {
		ErrPrint("Client is faulted\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	rpc = client_data(client, "rpc");
	if (!rpc) {
		ErrPrint("Client rpc data is not valid (%s)\n", funcname);
		g_variant_unref(param);
		return -EINVAL;
	}

	packet = create_packet(client, funcname, param);
	if (!packet) {
		g_variant_unref(param);
		return -ENOMEM;
	}

	if (!rpc->proxy)
		rpc->pending_request_list = eina_list_append(rpc->pending_request_list, packet);
	else
		push_packet(packet);

	return 0;
}

int client_rpc_broadcast(const char *funcname, GVariant *param)
{
	Eina_List *l;
	struct client_rpc *rpc;

	EINA_LIST_FOREACH(s_info.rpc_list, l, rpc) {
		(void)client_rpc_async_request(rpc->client, funcname, g_variant_ref(param));
	}

	g_variant_unref(param);
	return 0;
}

int client_rpc_sync_request(struct client_node *client, const char *funcname, GVariant *param)
{
	struct client_rpc *rpc;
	GError *err;
	GVariant *result;
	int ret;

	if (!client_is_activated(client)) {
		ErrPrint("Client is not activated (%s)\n", funcname);
		g_variant_unref(param);
		return -EINVAL;
	}

	if (client_is_faulted(client)) {
		ErrPrint("Client is faulted\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	rpc = client_data(client, "rpc");
	if (!rpc) {
		ErrPrint("Client has no \"rpc\" info (%s)\n", funcname);
		g_variant_unref(param);
		return -EINVAL;
	}

	if (!rpc->proxy) {
		ErrPrint("Client is not ready to communicate (%s)\n", funcname);
		g_variant_unref(param);
		return -EINVAL;
	}

	err = NULL;
	result = g_dbus_proxy_call_sync(rpc->proxy, funcname, param,
				G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, &err);

	if (!result) {
		if (err) {
			ErrPrint("(%s) Error: %s\n", funcname, err->message);
			g_error_free(err);
		}

		client_fault(client);
		return -EIO;
	}

	g_variant_get(result, "(i)", &ret);
	g_variant_unref(result);
	return ret;
}

int client_rpc_update_proxy(struct client_node *client, GDBusProxy *proxy)
{
	struct client_rpc *rpc;
	struct packet *packet;

	rpc = client_data(client, "rpc");
	if (!rpc) {
		ErrPrint("Client has no rpc info\n");
		return -EINVAL;
	}

	if (rpc->proxy)
		DbgPrint("Proxy is overwritten\n");

	rpc->proxy = proxy;

	EINA_LIST_FREE(rpc->pending_request_list, packet) {
		push_packet(packet);
	}

	return 0;
}

int client_rpc_update_conn(struct client_node *client, GDBusConnection *conn)
{
	struct client_rpc *rpc;

	rpc = client_data(client, "rpc");
	if (!rpc) {
		ErrPrint("client has no \"rpc\" info\n");
		return -EINVAL;
	}

	if (rpc->conn)
		ErrPrint("Connection is already exists, overwrite it\n");

	rpc->conn = conn;
	return 0;
}

static int deactivated_cb(struct client_node *client, void *data)
{
	struct client_rpc *rpc;
	struct packet *packet;
	Eina_List *l;
	Eina_List *n;

	rpc = client_data(client, "rpc");
	if (!rpc) {
		ErrPrint("client is not valid\n");
		return 0;
	}

	if (!rpc->proxy) {
		ErrPrint("RPC has no proxy\n");
		return 0;
	}

	g_object_unref(rpc->proxy);

	rpc->proxy = NULL;
	rpc->conn = NULL;

	EINA_LIST_FOREACH_SAFE(s_info.packet_list, l, n, packet) {
		if (packet->client != client)
			continue;

		s_info.packet_list = eina_list_remove(s_info.packet_list, packet);
		destroy_packet(packet);
	}

	return 0;
}

static int del_cb(struct client_node *client, void *data)
{
	struct client_rpc *rpc;
	struct packet *packet;

	rpc = client_del_data(client, "rpc");
	if (!rpc) {
		ErrPrint("RPC is not valid\n");
		return -EINVAL;
	}

	s_info.rpc_list = eina_list_remove(s_info.rpc_list, rpc);

	EINA_LIST_FREE(rpc->pending_request_list, packet) {
		destroy_packet(packet);
	}

	if (rpc->proxy)
		g_object_unref(rpc->proxy);

	free(rpc);

	client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, deactivated_cb, NULL);
	return -1; /* Return <0, Delete this callback */
}

int client_rpc_initialize(struct client_node *client)
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
		free(rpc);
		return ret;
	}

	client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, deactivated_cb, NULL);
	client_event_callback_add(client, CLIENT_EVENT_DESTROY, del_cb, NULL);
	rpc->client = client;

	s_info.rpc_list = eina_list_append(s_info.rpc_list, rpc);
	return 0;
}

struct client_node *client_rpc_find_by_proxy(GDBusProxy *proxy)
{
	Eina_List *l;
	struct client_rpc *rpc;

	EINA_LIST_FOREACH(s_info.rpc_list, l, rpc) {
		if (rpc->proxy != proxy)
			continue;

		return rpc->client;
	}

	return NULL;
}

struct client_node *client_rpc_find_by_conn(GDBusConnection *conn)
{
	Eina_List *l;
	struct client_rpc *rpc;

	EINA_LIST_FOREACH(s_info.rpc_list, l, rpc) {
		if (rpc->conn != conn)
			continue;

		return rpc->client;
	}

	return NULL;
}

/* End of a file */
