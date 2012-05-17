#include <stdio.h>
#include <stdlib.h> /* malloc */
#include <string.h> /* strdup */
#include <libgen.h> /* basename */

#include <Ecore.h> /* ecore_timer */

#include <gio/gio.h>

#include <dlog.h>

#include "debug.h"
#include "client_manager.h"
#include "slave_manager.h"
#include "pkg_manager.h"
#include "ctx_client.h"
#include "xmonitor.h"

int aul_listen_app_dead_signal(int (*)(int, void *), void *);

static struct info {
	Eina_List *client_list;
	int nr_of_clients;
	int nr_of_paused_clients;
} s_info = {
	.client_list = NULL,
	.nr_of_clients = 0,
	.nr_of_paused_clients = 0,
};

struct cmd_item {
	char *funcname;
	GVariant *param;
	struct client_node *client;
};

struct client_node {
	int pid;
	GDBusProxy *proxy;
	Ecore_Timer *cmd_timer;
	Eina_List *sending_list;
	Eina_List *pkg_list;
	int paused;
};

static inline void destroy_command(struct cmd_item *item)
{
	free(item->funcname);
	g_variant_unref(item->param);
	free(item);
}

static inline void clear_sending_list(struct client_node *client)
{
	struct cmd_item *item;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(client->sending_list, l, n, item) {
		client->sending_list = eina_list_remove_list(client->sending_list, l);
		destroy_command(item);
	}
}

static inline void destroy_client(struct client_node *client)
{
	if (client->paused)
		s_info.nr_of_paused_clients--;

	s_info.nr_of_clients--;

	pkgmgr_delete_by_client(client);
	clear_sending_list(client);

	if (client->cmd_timer)
		ecore_timer_del(client->cmd_timer);

	if (client->proxy)
		g_object_unref(client->proxy);

	free(client);

	slave_check_pause_or_resume();
}

static void client_cmd_done(GDBusProxy *proxy, GAsyncResult *res, void *_item)
{
	GVariant *result;
	GError *err;
	struct cmd_item *item;

	/* NOTE: Can we believe the item->param ? */
	item = _item;

	err = NULL;
	result = g_dbus_proxy_call_finish(proxy, res, &err);
	if (result) {
		int ret;

		g_variant_get(result, "(i)", &ret);
		g_variant_unref(result);

		destroy_command(item);
	} else {
		if (err) {
			if (eina_list_data_find(s_info.client_list, item->client))
				ErrPrint("Error: %s\n", err->message);

			g_error_free(err);

			client_destroy(item->client);
		} else {
			destroy_command(item);
		}
	}
}

static Eina_Bool cmd_consumer_cb(void *data)
{
	struct client_node *client = data;
	struct cmd_item *item;

	item = eina_list_nth(client->sending_list, 0);
	if (!item) {
		client->cmd_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	client->sending_list = eina_list_remove(client->sending_list, item);
	g_dbus_proxy_call(client->proxy, item->funcname, g_variant_ref(item->param),
						G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL,
						(GAsyncReadyCallback)client_cmd_done, item);

	if (!client->sending_list) {
		client->cmd_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	return ECORE_CALLBACK_RENEW;
}

static inline void check_and_fire_cmd_consumer(struct client_node *client)
{
	if (!client->proxy || client->cmd_timer || !client->sending_list)
		return;

	client->cmd_timer = ecore_timer_add(0.001f, cmd_consumer_cb, client);
	if (!client->cmd_timer)
		ErrPrint("Failed to append a command consumer\n");
}

static inline struct cmd_item *make_cmd(struct client_node *client, const char *funcname, GVariant *param)
{
	struct cmd_item *cmd;

	cmd = malloc(sizeof(*cmd));
	if (!cmd)
		return NULL;

	cmd->funcname = strdup(funcname);
	if (!cmd->funcname) {
		free(cmd);
		return NULL;
	}

	cmd->param = g_variant_ref(param);
	cmd->client = client;

	return cmd;
}

struct client_node *client_new(int pid)
{
	struct client_node *client;

	client = malloc(sizeof(*client));
	if (!client) {
		ErrPrint("Error: %s\n", strerror(errno));
		return NULL;
	}

	client->pid = pid;
	client->proxy = NULL;
	client->cmd_timer = NULL;
	client->sending_list = NULL;
	client->pkg_list = NULL;
	client->paused = 0;

	s_info.client_list = eina_list_append(s_info.client_list, client);
	s_info.nr_of_clients++;

	/*! \NOTE
	 * Pause the client first, and if it receives ACTIVATE signal,
	 * resume it
	 */
	client_pause(client);
	xmonitor_update_state();

	return client;
}

GDBusProxy *client_proxy(struct client_node *client)
{
	return client->proxy;
}

int client_pid(struct client_node *client)
{
	return client->pid;
}

struct client_node *client_find_by_connection(GDBusConnection *conn)
{
	struct client_node *client;
	Eina_List *l;
	GDBusProxy *proxy;

	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		proxy = client_proxy(client);
		if (g_dbus_proxy_get_connection(proxy) == conn)
			return client;
	}

	return NULL;
}

struct client_node *client_find(int pid)
{
	struct client_node *client;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		if (client_pid(client) == pid)
			return client;
	}

	return NULL;
}

int client_is_all_paused(void)
{
	DbgPrint("nr_of_clients: %d / nr_of_paused_clients: %d\n",
			s_info.nr_of_clients, s_info.nr_of_paused_clients);

	return s_info.nr_of_clients == s_info.nr_of_paused_clients;
}

void client_pause(struct client_node *client)
{
	if (!client->paused)
		s_info.nr_of_paused_clients++;

	client->paused = 1;
	slave_check_pause_or_resume();
}

void client_resume(struct client_node *client)
{
	if (client->paused)
		s_info.nr_of_paused_clients--;

	client->paused = 0;
	slave_check_pause_or_resume();
}

int client_destroy(struct client_node *client)
{
	if (!eina_list_data_find(s_info.client_list, client)) {
		ErrPrint("Client is not valid %p\n", client);
		return -ENOENT;
	}

	s_info.client_list = eina_list_remove(s_info.client_list, client);
	destroy_client(client);
	return 0;
}

int client_update_proxy(struct client_node *client, GDBusProxy *proxy)
{
	client->proxy = proxy;

	pkgmgr_inform_pkglist(client);
	check_and_fire_cmd_consumer(client);
	return 0;
}

int client_push_command(struct client_node *client, const char *funcname, GVariant *param)
{
	struct cmd_item *cmd;

	cmd = make_cmd(client, funcname, param);
	g_variant_unref(param);
	if (!cmd)
		return -ENOMEM;

	client->sending_list = eina_list_append(client->sending_list, cmd);
	check_and_fire_cmd_consumer(client);
	return 0;
}

int client_broadcast_command(const char *funcname, GVariant *param)
{
	struct cmd_item *cmd;
	Eina_List *l;
	struct client_node *client;

	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		cmd = make_cmd(client, funcname, param);
		if (cmd) {
			client->sending_list = eina_list_append(client->sending_list, cmd);
			check_and_fire_cmd_consumer(client);
		}
	}

	g_variant_unref(param);
	return 0;
}

int client_fault_deactivating(struct client_node *client)
{
	/*!
	 * \todo
	 * remove all pkg, which are created by this client!
	 */
	(void)client_destroy(client);
	return 0;
}

int client_manager_init(void)
{
	return 0;
}

int client_manager_fini(void)
{
	struct client_node *client;
	Eina_List *n;
	Eina_List *l;

	EINA_LIST_FOREACH_SAFE(s_info.client_list, l, n, client) {
		s_info.client_list = eina_list_remove_list(s_info.client_list, l);
		destroy_client(client);
	}

	return 0;
}

/* End of a file */
