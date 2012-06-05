#include <stdio.h>
#include <libgen.h>
#include <errno.h>

#include <Eina.h>

#include <dlog.h>
#include <gio/gio.h>

#include "client_life.h"
#include "client_rpc.h"
#include "debug.h"

int errno;

static struct {
	Eina_List *client_list;
	int nr_of_paused_clients;

	Eina_List *create_event_list;
	Eina_List *destroy_event_list;
} s_info = {
	.client_list = NULL,
	.nr_of_paused_clients = 0,
	.create_event_list = NULL,
	.destroy_event_list = NULL,
};

struct global_event_handler {
	void *cbdata;
	int (*cb)(struct client_node *client, void *data);
};

struct data_item {
	char *tag;
	void *data;
};

struct event_item {
	void *data;
	int (*cb)(struct client_node *, void *);
};

struct client_node {
	pid_t pid;
	int refcnt;

	int paused;

	Eina_List *event_deactivate_list;
	Eina_List *event_destroy_list;
	Eina_List *data_list;

	int faulted;
};

static inline void invoke_global_destroy_cb(struct client_node *client)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_handler *item;

	EINA_LIST_FOREACH_SAFE(s_info.destroy_event_list, l, n, item) {
		if (item->cb(client, item->cbdata) < 0) {
			if (eina_list_data_find(s_info.destroy_event_list, item)) {
				s_info.destroy_event_list = eina_list_remove(s_info.destroy_event_list, item);
				free(item);
			}
		}
	}
}

static inline void invoke_global_create_cb(struct client_node *client)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_handler *item;

	EINA_LIST_FOREACH_SAFE(s_info.create_event_list, l, n, item) {
		if (item->cb(client, item->cbdata) < 0) {
			if (eina_list_data_find(s_info.create_event_list, item)) {
				s_info.create_event_list = eina_list_remove(s_info.create_event_list, item);
				free(item);
			}
		}
	}
}

static inline void destroy_client_data(struct client_node *client)
{
	struct event_item *event;
	struct data_item *data;
	Eina_List *l;
	Eina_List *n;

	DbgPrint("Client %p is destroyed\n", client);

	invoke_global_destroy_cb(client);

	EINA_LIST_FOREACH_SAFE(client->event_destroy_list, l, n, event) {
		event->cb(client, event->data);
		if (eina_list_data_find(client->event_destroy_list, event)) {
			client->event_destroy_list = eina_list_remove(client->event_destroy_list, event);
			free(event);
		}
	}

	EINA_LIST_FREE(client->data_list, data) {
		free(data->tag);
		free(data);
	}

	EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, event) {
		client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, event);
		free(event);
	}

	if (client->paused)
		s_info.nr_of_paused_clients--;

	s_info.client_list = eina_list_remove(s_info.client_list, client);
	free(client);
}

static inline struct client_node *create_client_data(pid_t pid)
{
	struct client_node *client;

	client = calloc(1, sizeof(*client));
	if (!client) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	client->pid = pid;

	s_info.client_list = eina_list_append(s_info.client_list, client);
	return client;
}

struct client_node *client_create(pid_t pid)
{
	struct client_node *client;

	client = client_find_by_pid(pid);
	if (client)
		return client;

	client = create_client_data(pid);
	if (!client)
		return NULL;

	client_ref(client);
	client_rpc_initialize(client);

	invoke_global_create_cb(client);
	return client;
}

int client_destroy(struct client_node *client)
{
	client_unref(client);
	return 0;
}

struct client_node *client_ref(struct client_node *client)
{
	if (!client)
		return NULL;

	client->refcnt++;
	return client;
}

struct client_node *client_unref(struct client_node *client)
{
	if (!client)
		return NULL;

	if (client->refcnt == 0) {
		ErrPrint("Client refcnt is not managed correctly\n");
		return NULL;
	}

	client->refcnt--;
	if (client->refcnt == 0) {
		destroy_client_data(client);
		client = NULL;
	}

	return client;
}

int client_refcnt(struct client_node *client)
{
	return client->refcnt;
}

pid_t client_pid(struct client_node *client)
{
	return client ? client->pid : (pid_t)-1;
}

struct client_node *client_find_by_pid(pid_t pid)
{
	Eina_List *l;
	struct client_node *client;

	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		if (client->pid == pid)
			return client;
	}

	return NULL;
}

int client_count_paused(void)
{
	return s_info.nr_of_paused_clients;
}

int client_is_all_paused(void)
{
	return eina_list_count(s_info.client_list) == s_info.nr_of_paused_clients;
}

int client_count(void)
{
	return eina_list_count(s_info.client_list);
}

static inline void invoke_deactivated_cb(struct client_node *client)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;
	int ret;

	EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, item) {
		ret = item->cb(client, item->data);
		if (ret < 0) {
			if (eina_list_data_find(client->event_deactivate_list, item)) {
				client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, item);
				free(item);
			}
		}
	}
}

int client_deactivated_by_fault(struct client_node *client)
{
	if (client->pid != (pid_t)-1) {
		client->pid = (pid_t)-1;
		invoke_deactivated_cb(client);
	}

	client_unref(client);
	return 0;
}

int client_fault(struct client_node *client)
{
	DbgPrint("Client is faulted! %d\n", client_refcnt(client));
	client->faulted = 1;
	/*!
	 * \todo
	 * Who invokes this function has to care the reference counter of a client
	 * do I need to invoke the deactivated callback from here?
	 * client->pid = (pid_t)-1;
	 * slave_unref(client)
	 */
	return 0;
}

int client_is_faulted(struct client_node *client)
{
	/*!
	 * \note
	 * If the "client" is NIL, I assume that it is fault so returns TRUE(1)
	 */
	return client ? client->faulted : 1;
}

void client_reset_fault(struct client_node *client)
{
	client->faulted = 0;
}

int client_event_callback_add(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data)
{
	struct event_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	item->cb = cb;
	item->data = data;

	switch (event) {
	case CLIENT_EVENT_DEACTIVATE:
		client->event_deactivate_list = eina_list_append(client->event_deactivate_list, item);
		break;
	case CLIENT_EVENT_DESTROY:
		client->event_destroy_list = eina_list_append(client->event_destroy_list, item);
		break;
	default:
		free(item);
		return -EINVAL;
	}

	return 0;
}

int client_event_callback_del(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;

	switch (event) {
	case CLIENT_EVENT_DEACTIVATE:
		EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, item) {
			if (item->cb == cb && item->data == data) {
				client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, item);
				free(item);
				return 0;
			}
		}
		break;

	case CLIENT_EVENT_DESTROY:
		EINA_LIST_FOREACH_SAFE(client->event_destroy_list, l, n, item) {
			if (item->cb == cb && item->data == data) {
				client->event_destroy_list = eina_list_remove(client->event_destroy_list, item);
				free(item);
				return 0;
			}
		}
		break;

	default:
		ErrPrint("Invalid event\n");
		break;
	}

	return -ENOENT;
}

int client_set_data(struct client_node *client, const char *tag, void *data)
{
	struct data_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	item->tag = strdup(tag);
	if (!item->tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(item);
		return -ENOMEM;
	}

	item->data = data;

	client->data_list = eina_list_append(client->data_list, item);
	return 0;
}

void *client_data(struct client_node *client, const char *tag)
{
	Eina_List *l;
	struct data_item *item;

	EINA_LIST_FOREACH(client->data_list, l, item) {
		if (!strcmp(item->tag, tag))
			return item->data;
	}

	return NULL;
}

void *client_del_data(struct client_node *client, const char *tag)
{
	Eina_List *l;
	Eina_List *n;
	struct data_item *item;

	EINA_LIST_FOREACH_SAFE(client->data_list, l, n, item) {
		if (!strcmp(item->tag, tag)) {
			void *data;
			client->data_list = eina_list_remove(client->data_list, item);
			data = item->data;
			free(item->tag);
			free(item);
			return data;
		}
	}

	return NULL;
}

void client_paused(struct client_node *client)
{
	client->paused = 1;
	s_info.nr_of_paused_clients++;
}

void client_resumed(struct client_node *client)
{
	client->paused = 0;
	s_info.nr_of_paused_clients--;
}

int client_init(void)
{
	return 0;
}

int client_fini(void)
{
	struct client_node *client;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(s_info.client_list, l, n, client) {
		client_destroy(client);
	}

	return 0;
}

int client_is_activated(struct client_node *client)
{
	return client ? (client->pid != (pid_t)-1) : 1;
}

int client_global_event_handler_add(enum client_global_event event_type, int (*cb)(struct client_node *client, void *data), void *data)
{
	struct global_event_handler *handler;

	handler = malloc(sizeof(*handler));
	if (!handler)
		return -ENOMEM;

	handler->cbdata = data;
	handler->cb = cb;

	if (event_type == CLIENT_GLOBAL_EVENT_CREATE) {
		s_info.create_event_list = eina_list_append(s_info.create_event_list, handler);
	} else if (event_type == CLIENT_GLOBAL_EVENT_DESTROY) {
		s_info.destroy_event_list = eina_list_append(s_info.destroy_event_list, handler);
	} else {
		free(handler);
		return -EINVAL;
	}

	return 0;
}

int client_global_event_handler_del(enum client_global_event event_type, int (*cb)(struct client_node *, void *), void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_handler *handler;

	if (event_type == CLIENT_GLOBAL_EVENT_CREATE) {
		EINA_LIST_FOREACH_SAFE(s_info.create_event_list, l, n, handler) {
			if (handler->cb == cb && handler->cbdata == data) {
				s_info.create_event_list = eina_list_remove(s_info.create_event_list, handler);
				free(handler);
				return 0;
			}
		}
	} else if (event_type == CLIENT_GLOBAL_EVENT_DESTROY) {
		EINA_LIST_FOREACH_SAFE(s_info.destroy_event_list, l, n, handler) {
			if (handler->cb == cb && handler->cbdata == data) {
				s_info.destroy_event_list = eina_list_remove(s_info.destroy_event_list, handler);
				free(handler);
				return 0;
			}
		}
	}

	return -ENOENT;
}

/* End of a file */
