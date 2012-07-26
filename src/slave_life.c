#include <stdio.h>
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */
#include <pthread.h>
#include <malloc.h>

#include <Eina.h>
#include <Ecore.h>

#include <aul.h> /* aul_launch_app */
#include <dlog.h>
#include <bundle.h>
#include <sqlite3.h>

#include <packet.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "fault_manager.h"
#include "ctx_client.h"
#include "debug.h"
#include "conf.h"
#include "setting.h"
#include "util.h"
#include "abi.h"

int errno;

struct slave_node {
	char *name;
	char *abi;
	int secured;	/* Only A package(livebox) is loaded for security requirements */
	int refcnt;
	int fault_count;
	enum pause_state {
		/*!
		 * Launch the slave but not yet receives "hello" packet
		 */
		SLAVE_REQUEST_TO_LAUNCH,

		/*!
		 * \note
		 * Terminate the slave but not yet receives dead signal
		 */
		SLAVE_REQUEST_TO_TERMINATE,

		/*!
		 * \note
		 * No slave process exists, just slave object created
		 */
		SLAVE_TERMINATED,

		/*!
		 * \note
		 * State change request is sent,
		 */
		SLAVE_REQUEST_TO_PAUSE,
		SLAVE_REQUEST_TO_RESUME,

		/*!
		 * \note
		 * SLAVE_ACTIVATED = { SLAVE_PAUSED, SLAVE_RESUMED }
		 */
		SLAVE_PAUSED,
		SLAVE_RESUMED,
	} state;

	int loaded_instance;
	int loaded_package;

	pid_t pid;

	Eina_List *event_activate_list;
	Eina_List *event_deactivate_list;
	Eina_List *event_delete_list;

	Eina_List *data_list;

	int faulted;
};

struct event {
	struct slave_node *slave;

	int (*evt_cb)(struct slave_node *, void *);
	void *cbdata;
};

struct priv_data {
	char *tag;
	void *data;
};

static struct {
	Eina_List *slave_list;
	int paused;
} s_info = {
	.slave_list = NULL,
	.paused = 0,
};

static inline struct slave_node *create_slave_node(const char *name, int is_secured, const char *abi)
{
	struct slave_node *slave;

	slave = calloc(1, sizeof(*slave));
	if (!slave) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	slave->name = strdup(name);
	if (!slave->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(slave);
		return NULL;
	}

	slave->abi = strdup(abi);
	if (!slave->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(slave->name);
		free(slave);
		return NULL;
	}

	slave->secured = is_secured;
	slave->pid = (pid_t)-1;
	slave->state = SLAVE_TERMINATED;

	s_info.slave_list = eina_list_append(s_info.slave_list, slave);
	DbgPrint("slave data is created %p\n", slave);
	return slave;
}

static inline void invoke_delete_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_delete_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_delete_list, event)) {
				slave->event_delete_list = eina_list_remove(slave->event_delete_list, event);
				free(event);
			}
		}
	}
}

static inline void destroy_slave_node(struct slave_node *slave)
{
	struct event *event;
	struct priv_data *priv;

	if (slave->pid != (pid_t)-1) {
		ErrPrint("Slave is not deactivated\n");
		return;
	}

	DbgPrint("Slave data is destroyed %p\n", slave);

	invoke_delete_cb(slave);

	EINA_LIST_FREE(slave->event_delete_list, event) {
		free(event);
	}

	EINA_LIST_FREE(slave->event_activate_list, event) {
		free(event);
	}

	EINA_LIST_FREE(slave->event_deactivate_list, event) {
		free(event);
	}

	EINA_LIST_FREE(slave->data_list, priv) {
		free(priv->tag);
		free(priv);
	}

	s_info.slave_list = eina_list_remove(s_info.slave_list, slave);
	free(slave->abi);
	free(slave->name);
	free(slave);
	return;
}

static inline struct slave_node *find_slave(const char *name)
{
	struct slave_node *slave;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave->name, name))
			return slave;
	}
	
	return NULL;
}

struct slave_node *slave_ref(struct slave_node *slave)
{
	if (!slave)
		return NULL;

	slave->refcnt++;
	return slave;
}

struct slave_node *slave_unref(struct slave_node *slave)
{
	if (!slave)
		return NULL;

	if (slave->refcnt == 0) {
		ErrPrint("Slave refcnt is not valid\n");
		return NULL;
	}

	slave->refcnt--;
	if (slave->refcnt == 0) {
		destroy_slave_node(slave);
		slave = NULL;
	}

	return slave;
}

const int const slave_refcnt(struct slave_node *slave)
{
	return slave->refcnt;
}

struct slave_node *slave_create(const char *name, int is_secured, const char *abi)
{
	struct slave_node *slave;

	slave = find_slave(name);
	if (slave) {
		if (slave->secured != is_secured)
			ErrPrint("Exists slave and creating slave's security flag is not matched\n");
		return slave;
	}

	slave = create_slave_node(name, is_secured, abi);
	slave_ref(slave);

	return slave;
}

/*!
 * \note
 * Before destroying slave object,
 * you should check the RPC(slave_async_XXX) state and Private data field (slave_set_data)
 */
void slave_destroy(struct slave_node *slave)
{
	slave_unref(slave);
}

static inline void invoke_activate_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_activate_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_activate_list, event)) {
				slave->event_activate_list = eina_list_remove(slave->event_activate_list, event);
				free(event);
			}
		}
	}
}

const int const slave_is_faulted(const struct slave_node *slave)
{
	return slave->faulted;
}

int slave_activate(struct slave_node *slave)
{
	bundle *param;
	const char *slave_pkgname;

	if (slave->pid != (pid_t)-1)
		return -EALREADY;

	slave_pkgname = abi_find_slave(slave->abi);
	if (!slave_pkgname)
		return -ENOTSUP;

	param = bundle_create();
	if (!param) {
		ErrPrint("Failed to create a bundle\n");
		return -EFAULT;
	}

	bundle_add(param, BUNDLE_SLAVE_NAME, slave->name);
	bundle_add(param, BUNDLE_SLAVE_SECURED, slave->secured ? "true" : "false");
	bundle_add(param, BUNDLE_SLAVE_ABI, slave->abi);
	slave->pid = (pid_t)aul_launch_app(slave_pkgname, param);
	bundle_free(param);

	if (slave->pid < 0) {
		ErrPrint("Failed to launch a new slave %s (%d)\n", slave->name, slave->pid);
		slave->pid = (pid_t)-1;
		return -EFAULT;
	}
	DbgPrint("Slave launched %d for %s\n", slave->pid, slave->name);

	slave->state = SLAVE_REQUEST_TO_LAUNCH;
	/*!
	 * \note
	 * Increase the refcnt of a slave,
	 * To prevent from making an orphan(slave).
	 */
	slave_ref(slave);

	return 0;
}

int slave_activated(struct slave_node *slave)
{
	slave->state = SLAVE_RESUMED;
	invoke_activate_cb(slave);
	slave_handle_state_change();
	return 0;
}

static inline void invoke_deactivate_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;
	int reactivate = 0;

	EINA_LIST_FOREACH_SAFE(slave->event_deactivate_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_deactivate_list, event)) {
				slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, event);
				free(event);
			}
		} else if (ret == SLAVE_NEED_TO_REACTIVATE) {
			reactivate++;
		}
	}

	if (reactivate) {
		DbgPrint("Need to reactivate a slave\n");
		slave_activate(slave);
	}
}

int slave_deactivate(struct slave_node *slave)
{
	pid_t pid;

	if (!slave_is_activated(slave) || slave->faulted) {
		ErrPrint("Slave is already deactivated\n");
		return -EALREADY;
	}

	DbgPrint("Deactivate a slave: %d\n", slave->pid);
	/*!
	 * \todo
	 * check the return value of the aul_terminate_pid
	 */
	pid = slave->pid;
	slave->pid = (pid_t)-1;

	DbgPrint("Terminate PID: %d\n", pid);
	if (aul_terminate_pid(pid) < 0)
		ErrPrint("Terminate failed. pid %d\n", pid);

	invoke_deactivate_cb(slave);

	slave->state = SLAVE_TERMINATED;
	slave_unref(slave);
	return 0;
}

void slave_faulted(struct slave_node *slave)
{
	pid_t pid;
	if (slave->faulted || !slave_is_activated(slave))
		return;

	slave->faulted = 1;

	fault_check_pkgs(slave);
	pid = slave->pid;
	slave->pid = (pid_t)-1;
	slave->fault_count++;

	DbgPrint("Terminate PID: %d\n", pid);
	if (pid > 0 && aul_terminate_pid(pid) < 0)
		ErrPrint("Terminate failed, pid %d\n", pid);

	invoke_deactivate_cb(slave);
	slave->state = SLAVE_TERMINATED;
	slave_unref(slave);
}

void slave_reset_fault(struct slave_node *slave)
{
	slave->faulted = 0;
}

void slave_deactivated_by_fault(struct slave_node *slave)
{
	if (slave->faulted || !slave_is_activated(slave))
		return;

	slave->faulted = 1;

	(void)fault_check_pkgs(slave);

	slave->pid = (pid_t)-1;
	slave->fault_count++;

	invoke_deactivate_cb(slave);
	slave->state = SLAVE_TERMINATED;
	slave_unref(slave);
}

const int const slave_is_activated(struct slave_node *slave)
{
	return slave->pid != (pid_t)-1;
}

int slave_event_callback_add(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data)
{
	struct event *ev;

	ev = calloc(1, sizeof(*ev));
	if (!ev) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	ev->slave = slave;
	ev->cbdata = data;
	ev->evt_cb = cb;

	switch (event) {
	case SLAVE_EVENT_ACTIVATE:
		slave->event_activate_list = eina_list_append(slave->event_activate_list, ev);
		break;
	case SLAVE_EVENT_DELETE:
		slave->event_delete_list = eina_list_append(slave->event_delete_list, ev);
		break;
	case SLAVE_EVENT_DEACTIVATE:
		slave->event_deactivate_list = eina_list_append(slave->event_deactivate_list, ev);
		break;
	default:
		free(ev);
		return -EINVAL;
	}

	return 0;
}

int slave_event_callback_del(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data)
{
	struct event *ev;
	Eina_List *l;
	Eina_List *n;

	switch (event) {
	case SLAVE_EVENT_DEACTIVATE:
		EINA_LIST_FOREACH_SAFE(slave->event_deactivate_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, ev);
				free(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_DELETE:
		EINA_LIST_FOREACH_SAFE(slave->event_delete_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_delete_list = eina_list_remove(slave->event_delete_list, ev);
				free(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_ACTIVATE:
		EINA_LIST_FOREACH_SAFE(slave->event_activate_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_activate_list = eina_list_remove(slave->event_activate_list, ev);
				free(ev);
				return 0;
			}
		}
		break;
	default:
		return -EINVAL;
	}

	return -ENOENT;
}

int slave_set_data(struct slave_node *slave, const char *tag, void *data)
{
	struct priv_data *priv;

	priv = calloc(1, sizeof(*priv));
	if (!priv) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	priv->tag = strdup(tag);
	if (!priv->tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(priv);
		return -ENOMEM;
	}

	priv->data = data;
	slave->data_list = eina_list_append(slave->data_list, priv);
	return 0;
}

void *slave_del_data(struct slave_node *slave, const char *tag)
{
	struct priv_data *priv;
	void *data;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(slave->data_list, l, n, priv) {
		if (!strcmp(priv->tag, tag)) {
			slave->data_list = eina_list_remove(slave->data_list, priv);

			data = priv->data;
			free(priv->tag);
			free(priv);
			return data;
		}
	}

	return NULL;
}

void *slave_data(struct slave_node *slave, const char *tag)
{
	struct priv_data *priv;
	Eina_List *l;

	EINA_LIST_FOREACH(slave->data_list, l, priv) {
		if (!strcmp(priv->tag, tag))
			return priv->data;
	}

	return NULL;
}

struct slave_node *slave_find_by_pid(pid_t pid)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->pid == pid)
			return slave;
	}

	return NULL;
}

struct slave_node *slave_find_by_name(const char *name)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave->name, name))
			return slave;
	}

	return NULL;
}

struct slave_node *slave_find_available(void)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->secured)
			continue;

		DbgPrint("slave[%s] %d\n", slave_name(slave), slave->loaded_package);
		if (slave->loaded_package < g_conf.slave_max_load)
			return slave;
	}

	return NULL;
}

void slave_load_package(struct slave_node *slave)
{
	slave->loaded_package++;
}

void slave_unload_package(struct slave_node *slave)
{
	if (slave->loaded_package == 0) {
		ErrPrint("Slave loaded package is not correct\n");
		return;
	}
		
	slave->loaded_package--;
}

void slave_load_instance(struct slave_node *slave)
{
	slave->loaded_instance++;
	DbgPrint("Instance: (%d)%d\n", slave->pid, slave->loaded_instance);
}

int const slave_loaded_instance(struct slave_node *slave)
{
	return slave->loaded_instance;
}

void slave_unload_instance(struct slave_node *slave)
{
	if (slave->loaded_instance == 0) {
		ErrPrint("Slave loaded instance is not correct\n");
		return;
	}

	slave->loaded_instance--;
	DbgPrint("Instance: (%d)%d\n", slave->pid, slave->loaded_instance);
	if (slave->loaded_instance == 0)
		slave_deactivate(slave);
}

void slave_handle_state_change(void)
{
	int paused;
	Eina_List *l;
	struct slave_node *slave;

	paused = client_is_all_paused() || setting_is_lcd_off();

	if (s_info.paused == paused)
		return;

	s_info.paused = paused;

	if (s_info.paused) {
		EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
			slave_pause(slave);
		}

		sqlite3_release_memory(SQLITE_FLUSH_MAX);
		malloc_trim(0);
	} else {
		EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
			slave_resume(slave);
		}
	}
}

const int const slave_is_secured(const struct slave_node *slave)
{
	return slave->secured;
}

const char * const slave_name(const struct slave_node *slave)
{
	return slave->name;
}

const pid_t const slave_pid(const struct slave_node *slave)
{
	return slave->pid;
}

static void resume_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;

	if (!packet) {
		ErrPrint("Failed to change the state of the slave\n");
		slave->state = SLAVE_PAUSED;
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		return;
	}

	if (ret == 0) {
		slave->state = SLAVE_RESUMED;
		slave_rpc_ping_thaw(slave);
	}
}

static void pause_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;

	if (!packet) {
		ErrPrint("Failed to change the state of the slave\n");
		slave->state = SLAVE_RESUMED;
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		return;
	}

	if (ret == 0) {
		slave->state = SLAVE_PAUSED;
		slave_rpc_ping_freeze(slave);
	}
}

int slave_resume(struct slave_node *slave)
{
	double timestamp;
	struct packet *packet;

	if (slave->state == SLAVE_RESUMED || slave->state == SLAVE_REQUEST_TO_RESUME)
		return 0;

	timestamp = util_timestamp();

	packet = packet_create("resume", "d", timestamp);
	if (!packet) {
		ErrPrint("Failed to prepare param\n");
		return -EFAULT;
	}

	slave->state = SLAVE_REQUEST_TO_RESUME;
	return slave_rpc_async_request(slave, NULL, packet, resume_cb, NULL);
}

int slave_pause(struct slave_node *slave)
{
	double timestamp;
	struct packet *packet;

	if (slave->state == SLAVE_PAUSED || slave->state == SLAVE_REQUEST_TO_PAUSE)
		return 0;

	timestamp = util_timestamp();

	packet = packet_create("pause", "d", timestamp);
	if (!packet) {
		ErrPrint("Failed to prepare param\n");
		return -EFAULT;
	}

	slave->state = SLAVE_REQUEST_TO_PAUSE;
	return slave_rpc_async_request(slave, NULL, packet, pause_cb, NULL);
}

/* End of a file */
