#include <stdio.h>
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */
#include <libgen.h> /* basename */

#include <Eina.h>
#include <Ecore.h>

#include <aul.h> /* aul_launch_app */
#include <dlog.h>
#include <bundle.h>

#include <gio/gio.h> /* GDBusProxy */

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "fault_manager.h"
#include "ctx_client.h"
#include "debug.h"
#include "conf.h"
#include "setting.h"
#include "util.h"

int errno;

struct slave_node {
	char *name;
	int secured;	/* Only A package(livebox) is loaded for security requirements */
	int refcnt;
	int fault_count;
	int paused;

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

static inline struct slave_node *create_slave_node(const char *name, int is_secured)
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

	slave->secured = is_secured;
	slave->pid = (pid_t)-1;

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

int const slave_refcnt(struct slave_node *slave)
{
	return slave->refcnt;
}

struct slave_node *slave_create(const char *name, int is_secured)
{
	struct slave_node *slave;

	slave = find_slave(name);
	if (slave) {
		if (slave->secured != is_secured)
			ErrPrint("Exists slave and creating slave's security flag is not matched\n");
		return slave;
	}

	slave = create_slave_node(name, is_secured);
	slave_ref(slave);

	slave_rpc_initialize(slave);
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

int slave_is_faulted(struct slave_node *slave)
{
	return slave->faulted;
}

int slave_activate(struct slave_node *slave)
{
	bundle *param;

	if (slave->pid != (pid_t)-1)
		return -EALREADY;

	param = bundle_create();
	if (!param) {
		ErrPrint("Failed to create a bundle\n");
		return -EFAULT;
	}

	bundle_add(param, BUNDLE_SLAVE_NAME, slave->name);
	slave->pid = (pid_t)aul_launch_app(SLAVE_PKGNAME, param);
	bundle_free(param);

	if (slave->pid < 0) {
		ErrPrint("Failed to launch a new slave %s\n", slave->name);
		return -EFAULT;
	}
	DbgPrint("Slave launched %d\n", slave->pid);

	/*!
	 * \note
	 * Increase the refcnt of a slave,
	 * To prevent from making an orphan(slave).
	 */
	slave_ref(slave);
	invoke_activate_cb(slave);
	slave_check_pause_or_resume();

	slave->faulted = 0;
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

	if (reactivate)
		slave_activate(slave);
}

int slave_deactivate(struct slave_node *slave)
{
	pid_t pid;
	DbgPrint("Slave deactivate: %d\n", slave->pid);
	if (slave->pid == (pid_t)-1)
		return -EALREADY;

	/*!
	 * \todo
	 * check the return value of the aul_terminate_pid
	 */
	pid = slave->pid;
	slave->pid = -1;
	aul_terminate_pid(pid);

	invoke_deactivate_cb(slave);

	slave_unref(slave);
	return 0;
}

void slave_faulted(struct slave_node *slave)
{
	if (slave->pid == (pid_t)-1)
		return;

	aul_terminate_pid(slave->pid);
	/*!
	 * \note
	 * Now the dead signal will be raised up
	 */
}

void slave_deactivated_by_fault(struct slave_node *slave)
{
	(void)fault_check_pkgs(slave);

	slave->pid = (pid_t)-1;
	slave->fault_count++;
	slave->faulted = 1;

	invoke_deactivate_cb(slave);

	slave_unref(slave);
}

int slave_is_activated(struct slave_node *slave)
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

		if (slave->loaded_package == g_conf.slave_max_load)
			continue;

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

int slave_loaded_instance(struct slave_node *slave)
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

void slave_check_pause_or_resume(void)
{
	int paused;
	Eina_List *l;
	struct slave_node *slave;

	paused = (client_count() && client_is_all_paused()) || setting_is_locked();

	if (s_info.paused == paused)
		return;

	s_info.paused = paused;

	if (s_info.paused) {
		EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
			slave_pause(slave);
		}
	} else {
		EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
			slave_resume(slave);
		}

		ctx_update();
	}
}

int const slave_is_secured(struct slave_node *slave)
{
	return slave->secured;
}

const char *slave_name(struct slave_node *slave)
{
	return slave->name;
}

pid_t slave_pid(struct slave_node *slave)
{
	return slave->pid;
}

static void resume_cb(struct slave_node *slave, const char *func, GVariant *result, void *data)
{
	int ret;

	if (!result)
		return;

	g_variant_get(result, "(i)", &ret);
	if (ret == 0)
		slave->paused = 0;
}

static void pause_cb(struct slave_node *slave, const char *func, GVariant *result, void *data)
{
	int ret;

	if (!result)
		return;

	g_variant_get(result, "(i)", &ret);
	if (ret == 0)
		slave->paused = 1;
}

int slave_resume(struct slave_node *slave)
{
	double timestamp;
	GVariant *param;

	if (!slave->paused)
		return 0;

	timestamp = util_timestamp();

	param = g_variant_new("(d)", timestamp);
	if (!param) {
		ErrPrint("Failed to prepare param\n");
		return -EFAULT;
	}

	return slave_rpc_async_request(slave, NULL, NULL, "resume", param, resume_cb, NULL);
}

int slave_pause(struct slave_node *slave)
{
	double timestamp;
	GVariant *param;

	if (slave->paused)
		return 0;

	timestamp = util_timestamp();

	param = g_variant_new("(d)", timestamp);
	if (!param) {
		ErrPrint("Failed to prepare param\n");
		return -EFAULT;
	}

	return slave_rpc_async_request(slave, NULL, NULL, "pause", param, pause_cb, NULL);
}

/* End of a file */
