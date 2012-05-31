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

#include "debug.h"
#include "conf.h"
#include "slave_manager.h"
#include "client_manager.h"
#include "util.h"
#include "pkg_manager.h"
#include "fault_manager.h"
#include "rpc_to_slave.h"
#include "ctx_client.h"

int errno;

struct cmd_item {
	char *pkgname;
	char *filename;
	char *funcname;
	GVariant *param;
	struct slave_node *slave;

	void (*ret_cb)(const char *funcname, GVariant *result, void *cbdata);
	void *cbdata;
};

struct slave_node {
	enum {
		CREATED = 0xbeefbeef,
		DESTROYING = 0xdeadbeef,
		DESTROYED = 0xdeaddead,
	} state;
	char *name;
	unsigned long name_hash;
	pid_t pid;
	int is_secured;	/* Only A package(livebox) is loaded for security requirements */
	GDBusProxy *proxy; /* To communicate with this slave */
	int refcnt;
	Ecore_Timer *cmd_timer;
	int fault_count;
	Ecore_Timer *pong_timer;
	int paused;

	Eina_List *sending_list;
	Eina_List *waiting_list;
};

struct deactivate_cb {
	int (*cb)(struct slave_node *slave, void *data);
	void *data;
};

static struct info {
	Eina_List *slave_list;
	Eina_List *deactivate_cb_list;
	int paused;
} s_info = {
	.slave_list = NULL,
	.deactivate_cb_list = NULL,
	.paused = 0,
};

static inline void pause_slave(struct slave_node *slave)
{
	double timestamp;
	GVariant *param;

	timestamp = util_get_timestamp();

	param = g_variant_new("(d)", timestamp);
	if (!param) {
		ErrPrint("Failed to prepare param\n");
		return;
	}

	(void)slave_push_command(slave, NULL, NULL, "pause", param, NULL, NULL);
}

static inline void resume_slave(struct slave_node *slave)
{
	double timestamp;
	GVariant *param;


	timestamp = util_get_timestamp();

	param = g_variant_new("(d)", timestamp);
	if (!param) {
		ErrPrint("Failed to prepare param\n");
		return;
	}

	(void)slave_push_command(slave, NULL, NULL, "resume", param, NULL, NULL);
}

static inline struct slave_node *create_slave_data(const char *name)
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

	slave->name_hash = util_string_hash(name);
	slave->pid = (pid_t)-1;
	slave->paused = s_info.paused;
	slave->state = CREATED;

	return slave;
}

static inline void destroy_command(struct cmd_item *item)
{
	if (item->pkgname)
		free(item->pkgname);
	if (item->filename)
		free(item->filename);
	if (item->funcname)
		free(item->funcname);

	g_variant_unref(item->param);
	free(item);
}

/*!
 * \note
 * This function has not to destroy command instance
 * beacuse the slave_done_cb will be invoked after connection is lost.
 * then it will try to access this smd_item.
 *
 * So from this function, we have to do not delete it.
 * just reset the "slave"(slave) instance pointer.
 *
 * and the slave_cmd_done function should check it and handle it properly.
 */
static inline void clear_waiting_command_list(struct slave_node *slave)
{
	struct cmd_item *item;

	EINA_LIST_FREE(slave->waiting_list, item) {
		item->slave = NULL;
	}
}

/*!
 * \note
 * This function clear all pended command requests,
 * so we can destroy every items in this list.
 * because those are not used by anyone.
 */
static inline void clear_sending_command_list(struct slave_node *slave)
{
	struct cmd_item *item;

	EINA_LIST_FREE(slave->sending_list, item) {
		destroy_command(item);
	}
}

void slave_destroyed(struct slave_node *slave)
{
	s_info.slave_list = eina_list_remove(s_info.slave_list, slave);

	pkgmgr_clear_slave_info(slave);

	if (slave->pong_timer)
		ecore_timer_del(slave->pong_timer);

	if (slave->proxy)
		g_object_unref(slave->proxy);

	if (slave->cmd_timer)
		ecore_timer_del(slave->cmd_timer);

	slave->state = DESTROYED;
	free(slave->name);
	free(slave);
}

static inline void invoke_deactivate_cbs(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct deactivate_cb *cb_data;
	int ret;

	EINA_LIST_FOREACH_SAFE(s_info.deactivate_cb_list, l, n, cb_data) {
		ret = cb_data->cb(slave, cb_data->data);
		if (ret == EXIT_FAILURE) {
			s_info.deactivate_cb_list = eina_list_remove_list(s_info.deactivate_cb_list, l);
			free(cb_data);
		}
	}
}

static Eina_Bool slave_pong_cb(void *data)
{
	struct slave_node *slave = data;

	ErrPrint("Slave is not responded in %lf secs\n", g_conf.ping_time);
	slave_fault_deactivating(slave, 1);

	slave->pong_timer = NULL;
	return ECORE_CALLBACK_CANCEL;
}

static void slave_cmd_done(GDBusProxy *proxy, GAsyncResult *res, void *_cmd_item)
{
	GVariant *result;
	GError *err;
	struct slave_node *slave;
	struct cmd_item *item;

	item = _cmd_item;
	slave = item->slave;
	if (!slave || slave->state == DESTROYED) {
		ErrPrint("Slave is already destroyed\n");
		goto out;
	}

	slave->waiting_list = eina_list_remove(slave->waiting_list, item);

	err = NULL;
	result = g_dbus_proxy_call_finish(proxy, res, &err);
	if (!result) {
		if (err) {
			ErrPrint("Error: %s\n", err->message);
			g_error_free(err);
		}

		slave_fault_deactivating(slave, 1);
		goto out;
	}

	if (item->ret_cb)
		item->ret_cb(item->funcname, result, item->cbdata);
	else
		g_variant_unref(result);

out:
	destroy_command(item);
	if (slave && !slave->waiting_list && slave->state == DESTROYING) {
		DbgPrint("All requests are cleared. Destroy this\n");
		slave_deactivate(slave);
	}
}

/*!
 * \note
 * This callback function will consume items in the sending list.
 */
static Eina_Bool cmd_consumer_cb(void *data)
{
	struct slave_node *slave = data;
	struct cmd_item *item;

	item = eina_list_nth(slave->sending_list, 0);
	if (!item) {
		slave->cmd_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	slave->sending_list = eina_list_remove(slave->sending_list, item);

	if (item->pkgname && pkgmgr_is_fault(item->pkgname)) {
		destroy_command(item);
	} else {
		slave->waiting_list = eina_list_append(slave->waiting_list, item);

		g_dbus_proxy_call(slave->proxy, item->funcname, g_variant_ref(item->param),
			G_DBUS_CALL_FLAGS_NO_AUTO_START,
			-1, NULL, (GAsyncReadyCallback)slave_cmd_done, item);
	}

	if (!slave->sending_list) {
		slave->cmd_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	return ECORE_CALLBACK_RENEW;
}

static inline void check_and_fire_cmd_consumer(struct slave_node *slave)
{
	if (!slave->proxy || slave->cmd_timer || !slave->sending_list)
		return;

	slave->cmd_timer = ecore_timer_add(0.001f, cmd_consumer_cb, slave);
	if (!slave->cmd_timer)
		ErrPrint("Failed to append a command consumer\n");
}

static void on_signal(GDBusProxy *proxy, gchar *sender, gchar *signame, GVariant *param, gpointer user_data)
{
	DbgPrint("Signal [%s] from [%s]\n", signame, sender);
}

static inline void register_signal_callback(struct slave_node *data)
{
	char *signal;
	int signal_len;
	int ret;

	signal_len = strlen(data->name) + strlen("signal:") + 1;

	signal = malloc(signal_len);
	if (!signal) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return;
	}

	ret = snprintf(signal, signal_len, "signal:%s", data->name);
	if (ret < 0) {
		ErrPrint("ret: %d\n", ret);
		goto out;
	}

	g_signal_connect(data->proxy, "g-signal", G_CALLBACK(on_signal), NULL);

out:
	free(signal);
	return;
}

static void renew_ret_cb(const char *funcname, GVariant *result, void *data)
{
	int ret;

	g_variant_get(result, "(i)", &ret);
	if (ret == 0)
		return;

	/*!
	 * \note
	 * Failed to re-create an instance.
	 * In this case, delete the instance and send its deleted status to every clients.
	 */
	ErrPrint("Failed to recreate, send delete event to clients (%d)\n", ret);
	pkgmgr_deleted(pkgmgr_name(data), pkgmgr_filename(data));
}

/* Prepare package create list for package auto creation */
static int prepare_sending_cb(struct slave_node *slave, struct inst_info *inst, void *data)
{
	const char *pkgname;

	pkgname = pkgmgr_name(inst);

	if (pkgmgr_is_fault(pkgname)) {
		pkgmgr_deleted(pkgname, pkgmgr_filename(inst));
		return EXIT_SUCCESS;
	}

	rpc_send_renew(inst, renew_ret_cb, inst);
	return EXIT_SUCCESS;
}

int slave_fault_deactivating(struct slave_node *slave, int terminate)
{
	if (!slave || !slave->proxy)
		return 0;

	if (slave->state == DESTROYING) {
		slave_destroyed(slave);
		return 0;
	}

	(void)fault_check_pkgs(slave);

	if (slave->cmd_timer) {
		ecore_timer_del(slave->cmd_timer);
		slave->cmd_timer = NULL;
	}

	if (terminate)
		slave_deactivate(slave);
	else
		slave->pid = (pid_t)-1;

	slave->proxy = NULL;
	slave->fault_count++;

	if (slave->pong_timer) {
		ecore_timer_del(slave->pong_timer);
		slave->pong_timer = NULL;
	}

	/*!
	 * \todo:
	 * Keep all requests to send them again when the slave is re-activated or just discard all?
	 * I just discarding all requests.
	 * I think, The livebox couldn't keep its context when it is re-created from crashes of other liveboxes
	 */
	clear_waiting_command_list(slave);
	clear_sending_command_list(slave);

	pkgmgr_renew_by_slave(slave, prepare_sending_cb, NULL);

	if (slave->state == CREATED)
		invoke_deactivate_cbs(slave);
	else
		slave_destroyed(slave);

	return 0;
}

int slave_update_proxy(struct slave_node *slave, GDBusProxy *proxy)
{
	if (!slave)
		return -EINVAL;

	if (slave->proxy)
		return -EBUSY;

	slave->proxy = proxy;
	register_signal_callback(slave);

	if (slave->pong_timer) {
		ErrPrint("Slave %s has pong-timer already!\n", slave->name);
		ecore_timer_del(slave->pong_timer);
	}

	slave->pong_timer = ecore_timer_add(g_conf.ping_time, slave_pong_cb, slave);
	if (!slave->pong_timer)
		ErrPrint("Failed to add pong timer %s\n", slave->name);

	check_and_fire_cmd_consumer(slave);
	return 0;
}

GDBusProxy *slave_get_proxy(struct slave_node *slave)
{
	return slave->proxy;
}

int slave_is_activated(struct slave_node *slave)
{
	return slave->pid > 0;
}

/*!
 * \note
 * Launch a new slave with bundle (its name)
 */
int slave_activate(struct slave_node *slave)
{
	bundle *param;

	if (slave->state != CREATED) {
		DbgPrint("$$$$$$$$$$$$ SLAVE is now destroying or destroyed, don't try to activate this\n");
		return -EINVAL;
	}

	if (slave->pid > 0)
		return -EBUSY;

	param = bundle_create();
	if (!param) {
		ErrPrint("Failed to create a bundle\n");
		return -EFAULT;
	}

	bundle_add(param, "name", slave->name);
	slave->pid = (pid_t)aul_launch_app(SLAVE_PKGNAME, param);
	bundle_free(param);

	if (slave->pid < 0)
		return -EFAULT;

	if (slave->paused)
		pause_slave(slave);

	return 0;
}

int slave_deactivate(struct slave_node *slave)
{
	if (slave->pid > 0) {
		aul_terminate_pid(slave->pid);
	}

	return 0;
}

/*!
 * \note
 * Increase the reference counter of the slave
 * Reference counter means number of loaded packages.
 * If there is no pakcage are loaded, the refcnt will be 0
 */
int slave_ref(struct slave_node *slave)
{
	if (slave->is_secured && slave->refcnt > 0) {
		ErrPrint("Secured slave could not load one more liveboxes : %d\n", slave->refcnt);
		return -EINVAL;
	}

	slave->refcnt++;
	return slave->refcnt;
}

/*!
 * \note
 * Decrease the reference counter of the slave.
 */
int slave_unref(struct slave_node *slave)
{
	if (slave->refcnt == 0)
		return -EINVAL;

	slave->refcnt--;
	return slave->refcnt;
}

int slave_refcnt(struct slave_node *slave)
{
	return slave->refcnt;
}

struct slave_node *slave_create(const char *name, int secured)
{
	struct slave_node *slave;

	slave = slave_find(name);
	if (slave)
		return slave;

	slave = create_slave_data(name);
	if (!slave) {
		ErrPrint("Failed to create a slave slave\n");
		return NULL;
	}

	if (slave_activate(slave) < 0) {
		ErrPrint("Launch failed: %s\n", name);
		slave_destroyed(slave);
		return NULL;
	}

	slave->is_secured = !!secured;
	s_info.slave_list = eina_list_append(s_info.slave_list, slave);
	return slave;
}

struct slave_node *slave_find_usable(void)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->is_secured && slave->refcnt > 0)
			continue;

		/*!
		 * \note
		 * Check the loaded package counter.
		 * Maximum loadable package counter is sotred in the g_conf.slave_max_load.
		 */
		if (slave->refcnt >= g_conf.slave_max_load)
			continue;

		return slave;
	}

	return NULL;
}

struct slave_node *slave_find_by_pid(pid_t pid)
{
	struct slave_node *slave;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->pid == pid)
			return slave;
	}

	return NULL;
}

struct slave_node *slave_find(const char *name)
{
	struct slave_node *slave;
	Eina_List *l;
	unsigned long hash;

	hash = util_string_hash(name);

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->name_hash == hash && !strcmp(slave->name, name))
			return slave;
	}

	return NULL;
}

const char *slave_name(struct slave_node *slave)
{
	return slave->name;
}

void slave_broadcast_command(const char *cmd, GVariant *param)
{
	struct slave_node *slave;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		(void)slave_push_command(slave, NULL, NULL, cmd, g_variant_ref(param), NULL, NULL);
	}

	g_variant_unref(param);
}

int slave_push_command(struct slave_node *slave, const char *pkgname, const char *filename, const char *cmd, GVariant *param, void (*ret_cb)(const char *funcname, GVariant *result, void *data), void *data)
{
	struct cmd_item *item;

	if (!slave || slave->state != CREATED) {
		g_variant_unref(param);
		return -EINVAL;
	}

	item = malloc(sizeof(*item));
	if (!item) {
		g_variant_unref(param);
		return -ENOMEM;
	}

	if (pkgname) {
		item->pkgname = strdup(pkgname);
		if (!item->pkgname) {
			ErrPrint("Heap: %s (%s)\n", strerror(errno), pkgname);
			g_variant_unref(param);
			free(item);
			return -ENOMEM;
		}
	} else {
		item->pkgname = NULL;
	}

	if (filename) {
		item->filename = strdup(filename);
		if (!item->filename) {
			ErrPrint("Heap: %s (%s)\n", strerror(errno), filename);
			g_variant_unref(param);
			free(item->pkgname);
			free(item);
			return -ENOMEM;
		}
	} else {
		item->filename = NULL;
	}

	item->funcname = strdup(cmd);
	if (!item->funcname) {
		ErrPrint("Heap: %s (%s)\n", strerror(errno), cmd);
		g_variant_unref(param);
		free(item->filename);
		free(item->pkgname);
		free(item);
		return -ENOMEM;
	}
	item->param = param;
	item->slave = slave;

	item->ret_cb = ret_cb;
	item->cbdata = data;

	/*!
	 * \note
	 * Add this item to sending_list, and fire the command consumer.
	 * Command consumer will comsume this requests
	 */
	slave->sending_list = eina_list_append(slave->sending_list, item);
	check_and_fire_cmd_consumer(slave);
	return 0;
}

int slave_destroy(struct slave_node *slave)
{
	DbgPrint("$$$$$$$$$$ Destroy slave\n");
	slave->state = DESTROYING;

	/*!
	 * \note
	 * Slave has to be destroyed,
	 * so clear pended requests
	 */
	clear_sending_command_list(slave);
	if (slave->waiting_list) {
		/*!
		 * \note
		 * If there is waiting packets
		 * Do not destory this slave now
		 * Wait until receive all those waitings
		 */
		return 0;
	}

	slave_deactivate(slave);
	return 0;
}

int slave_add_deactivate_cb(int (*cb)(struct slave_node *slave, void *data), void *data)
{
	struct deactivate_cb *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -ENOMEM;
	}

	item->cb = cb;
	item->data = data;

	s_info.deactivate_cb_list = eina_list_append(s_info.deactivate_cb_list, item);
	return 0;
}

void *slave_del_deactivate_cb(int (*cb)(struct slave_node *, void *))
{
	Eina_List *l;
	Eina_List *n;
	struct deactivate_cb *item;
	void *data;

	EINA_LIST_FOREACH_SAFE(s_info.deactivate_cb_list, l, n, item) {
		if (item->cb == cb) {
			s_info.deactivate_cb_list = eina_list_remove_list(s_info.deactivate_cb_list, l);
			data = item->data;
			free(item);
			return data;
		}
	}

	return NULL;
}

int slave_fault_count(struct slave_node *slave)
{
	return slave->fault_count;
}

int slave_manager_init(void)
{
	return 0;
}

int slave_manager_fini(void)
{
	struct slave_node *slave;

	EINA_LIST_FREE(s_info.slave_list, slave) {
		clear_waiting_command_list(slave);
		clear_sending_command_list(slave);

		slave_deactivate(slave);
		slave_destroyed(slave);
	}

	return 0;
}

void slave_ping(struct slave_node *slave)
{
	if (slave->pong_timer)
		ecore_timer_reset(slave->pong_timer);
}

void slave_reset_pid(struct slave_node *slave)
{
	/*!
	 * \note
	 * Just reset the PID.
	 * This function is used for preventing detection from dead callback.
	 * When process is terminated(dead), aul_dead_cb will be called.
	 *
	 * Currently, if we can find the process using terminated pid from the dead_cb,
	 * It will try to re-activate it (only for slave).
	 *
	 * To prevent it, reset the PID info of a slave.
	 */
	slave->pid = (pid_t)-1;
}

void slave_set_pid(struct slave_node *slave, pid_t pid)
{
	slave->pid = pid;
}

int slave_pid(struct slave_node *slave)
{
	return slave->pid;
}

void slave_pause(struct slave_node *slave)
{
	if (slave->paused)
		return;

	pause_slave(slave);
	slave->paused = 1;
}

void slave_resume(struct slave_node *slave)
{
	if (!slave->paused)
		return;

	resume_slave(slave);
	slave->paused = 0;
}
 
void slave_check_pause_or_resume(void)
{
	int paused;

	paused = client_is_all_paused();

	if (s_info.paused == paused)
		return;

	s_info.paused = paused;

	if (s_info.paused) {
		Eina_List *l;
		struct slave_node *slave;

		EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
			slave_pause(slave);
		}
	} else {
		Eina_List *l;
		struct slave_node *slave;

		EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
			slave_resume(slave);
		}

		ctx_update();
	}
}

int slave_is_secured(struct slave_node *slave)
{
	return slave->is_secured;
}

/* End of a file */
