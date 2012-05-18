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

struct cmd_item {
	char *pkgname;
	char *filename;
	char *funcname;
	GVariant *param;
	struct slave_node *slave;

	void (*ret_cb)(const char *funcname, GVariant *param, int ret, void *cbdata);
	void *cbdata;
};

struct slave_node {
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

	DbgPrint("Pause slave %d\n", slave_pid(slave));

	timestamp = util_get_timestamp();

	param = g_variant_new("(d)", timestamp);
	if (!param) {
		ErrPrint("Failed to prepare param\n");
		return;
	}

	slave_push_command(slave, NULL, NULL, "pause", param, NULL, NULL);
}

static inline void resume_slave(struct slave_node *slave)
{
	double timestamp;
	GVariant *param;

	DbgPrint("Resume slave %d\n", slave_pid(slave));

	timestamp = util_get_timestamp();

	param = g_variant_new("(d)", timestamp);
	if (!param) {
		ErrPrint("Failed to prepare param\n");
		return;
	}

	slave_push_command(slave, NULL, NULL, "resume", param, NULL, NULL);
}

static Eina_Bool slave_pong_cb(void *data)
{
	struct slave_node *slave = data;

	slave_fault_deactivating(slave, 1);

	slave->pong_timer = NULL;
	return ECORE_CALLBACK_CANCEL;
}

static inline struct slave_node *create_slave_data(const char *name)
{
	struct slave_node *slave;

	slave = malloc(sizeof(*slave));
	if (!slave) {
		ErrPrint("Error: %s\n", strerror(errno));
		return NULL;
	}

	slave->name = strdup(name);
	if (!slave->name) {
		ErrPrint("Error: %s\n", strerror(errno));
		free(slave);
		return NULL;
	}

	slave->pong_timer = NULL;
	slave->refcnt = 0;
	slave->name_hash = util_string_hash(name);
	slave->is_secured = 0;
	slave->proxy = NULL;
	slave->sending_list = NULL;
	slave->waiting_list = NULL;
	slave->cmd_timer = NULL;
	slave->fault_count = 0;
	slave->pid = (pid_t)-1;
	slave->paused = s_info.paused;

	if (slave_activate(slave) < 0) {
		ErrPrint("Launch failed: %s\n", name);
		free(slave->name);
		free(slave);
		return NULL;
	}

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
 * \note This function has not to destroy command instance
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
	Eina_List *l;
	Eina_List *n;
	struct cmd_item *item;

	EINA_LIST_FOREACH_SAFE(slave->waiting_list, l, n, item) {
		slave->waiting_list = eina_list_remove_list(slave->waiting_list, l);
		item->slave = NULL;
	}
}

static inline void clear_sending_command_list(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct cmd_item *item;

	EINA_LIST_FOREACH_SAFE(slave->sending_list, l, n, item) {
		slave->sending_list = eina_list_remove_list(slave->sending_list, l);
		destroy_command(item);
	}
}

static inline void destroy_slave_data(struct slave_node *slave)
{
	if (slave->pong_timer)
		ecore_timer_del(slave->pong_timer);

	clear_waiting_command_list(slave);
	clear_sending_command_list(slave);

	if (slave->pid > 0) {
		DbgPrint("Terminate pid %d (slave: %s)\n", slave->pid, slave->name);
		aul_terminate_pid(slave->pid);
		slave->pid = (pid_t)-1;
	}

	if (slave->proxy)
		g_object_unref(slave->proxy);

	free(slave->name);
	free(slave);
}

static inline Eina_List *find_slave(const char *name)
{
	return NULL;
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

static void slave_cmd_done(GDBusProxy *proxy, GAsyncResult *res, void *_cmd_item)
{
	GVariant *result;
	GError *err;
	struct slave_node *slave;
	struct cmd_item *item;
	const int ret;

	item = _cmd_item;
	slave = item->slave;

	err = NULL;
	result = g_dbus_proxy_call_finish(proxy, res, &err);
	if (!result) {
		if (err) {
			ErrPrint("Error: %s\n", err->message);
			g_error_free(err);
		}

		slave_fault_deactivating(slave, 1);
		destroy_command(item);
		return;
	}

	if (!slave) {
		destroy_command(item);
		g_variant_unref(result);
		return;
	}

	slave->waiting_list = eina_list_remove(slave->waiting_list, item);

	if (!strcmp(item->funcname, "new")) {
		struct inst_info *inst;
		double priority;
		int w, h;

		g_variant_get(result, "(iiid)", &ret, &w, &h, &priority);

		inst = pkgmgr_find(item->pkgname, item->filename);
		if (inst) {
			/*!
			 * \note
			 * ret == 0 : need_to_create 0
			 * ret == 1 : need_to_create 1
			 */
			DbgPrint("\"new\" method returns: %d\n", ret);
			if (ret == 0 || ret == 1) {
				pkgmgr_set_info(inst, w, h, priority);
				pkgmgr_created(item->pkgname, item->filename);
			} else {
				/*\note
				 * If the current instance is created by the client,
				 * send the deleted event or just delete an instance in the master
				 * It will be cared by the "create_ret_cb"
				 */
				struct client_node *client;

				client = pkgmgr_client(inst);
				if (client) {
					GVariant *param;
					/* Okay, the client wants to know about this */
					param = g_variant_new("(ss)", item->pkgname, item->filename);
					if (param)
						client_push_command(client, "deleted", param);
				}

				pkgmgr_delete(inst);
			}
		}
	} else {
		g_variant_get(result, "(i)", &ret);

		if (!strcmp(item->funcname, "delete")) {
			if (ret == 0)
				pkgmgr_deleted(item->pkgname, item->filename);
			else
				ErrPrint("%s is not deleted - returns %d\n", item->pkgname, ret);
		}
	}

	g_variant_unref(result);

	if (item->ret_cb)
		item->ret_cb(item->funcname, g_variant_ref(item->param), ret, item->cbdata);

	destroy_command(item);
}

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
		ErrPrint("Memory: %s\n", strerror(errno));
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

/* Prepare package create list for package auto creation */
static int prepare_sending_cb(struct slave_node *slave, struct inst_info *inst, void *data)
{
	const char *pkgname;
	GVariant *param;

	pkgname = pkgmgr_name(inst);

	if (pkgmgr_is_fault(pkgname)) {
		pkgmgr_deleted(pkgname, pkgmgr_filename(inst));
		return EXIT_SUCCESS;
	}

	/* Just send delete event to create again */
	param = g_variant_new("(ss)", pkgname, pkgmgr_filename(inst));
	if (param)
		client_broadcast_command("deleted", param);

	rpc_send_new(inst, NULL, NULL, 0);
	return EXIT_SUCCESS;
}

int slave_fault_deactivating(struct slave_node *slave, int terminate)
{
	if (!slave || !slave->proxy)
		return 0;

	(void)fault_check_pkgs(slave);

	if (slave->cmd_timer) {
		ecore_timer_del(slave->cmd_timer);
		slave->cmd_timer = NULL;
	}

	if (slave->pid > 0 && terminate)
		aul_terminate_pid(slave->pid);

	slave->proxy = NULL;
	slave->pid = (pid_t)-1;
	slave->fault_count++;
	if (slave->pong_timer) {
		ecore_timer_del(slave->pong_timer);
		slave->pong_timer = NULL;
	}

	/*
	 * TODO: Keep all command and send them again,
	 * or just clean all command when the slave crashes?
	 */
	clear_waiting_command_list(slave);
	clear_sending_command_list(slave);

	pkgmgr_renew_by_slave(slave, prepare_sending_cb, NULL);

	invoke_deactivate_cbs(slave);
	return 0;
}

int slave_update_proxy(struct slave_node *slave, GDBusProxy *proxy)
{
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

int slave_activate(struct slave_node *slave)
{
	bundle *param;

	if (slave->pid > 0)
		return -EBUSY;

	param = bundle_create();
	if (!param)
		return -EFAULT;

	bundle_add(param, "name", slave->name);
	slave->pid = (pid_t)aul_launch_app(SLAVE_PKGNAME, param);
	bundle_free(param);

	DbgPrint("Slave %s reactivated with pid %d\n", slave->name, slave->pid);

	if (slave->pid < 0)
		return -EFAULT;

	if (slave->paused)
		pause_slave(slave);

	return 0;
}

/* Laucnh the slave */
int slave_ref(struct slave_node *slave)
{
	if (slave->is_secured && slave->refcnt > 0) {
		ErrPrint("Secured slave could not load one more liveboxes : %d\n", slave->refcnt);
		return -EINVAL;
	}

	slave->refcnt++;
	return slave->refcnt;
}

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
	if (slave) {
		DbgPrint("Creating slave but found already exists one\n");
		return slave;
	}

	slave = create_slave_data(name);
	if (!slave) {
		ErrPrint("Failed to create a slave slave\n");
		return NULL;
	}

	slave->is_secured = !!secured;
	DbgPrint(">>>>>>>>> Add slave %d to slave_list\n", slave->pid);
	s_info.slave_list = eina_list_append(s_info.slave_list, slave);
	return slave;
}

struct slave_node *slave_find_usable(void)
{
	Eina_List *l;
	struct slave_node *slave;
	struct slave_node *tmp;

	slave = NULL;
	EINA_LIST_FOREACH(s_info.slave_list, l, tmp) {
		if (tmp->is_secured && tmp->refcnt > 0)
			continue;

		if (tmp->refcnt >= g_conf.slave_max_load)
			continue;

		slave = tmp;
	}

	return slave;
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

int slave_broadcast_command(const char *cmd, GVariant *param)
{
	struct slave_node *slave;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		slave_push_command(slave, NULL, NULL, cmd, g_variant_ref(param), NULL, NULL);
	}

	g_variant_unref(param);
	return 0;
}

int slave_push_command(struct slave_node *slave, const char *pkgname, const char *filename, const char *cmd, GVariant *param, void (*ret_cb)(const char *funcname, GVariant *param, int ret, void *data), void *data)
{
	struct cmd_item *item;

	item = malloc(sizeof(*item));
	if (!item)
		return -ENOMEM;

	if (pkgname) {
		item->pkgname = strdup(pkgname);
		if (!item->pkgname) {
			free(item);
			return -ENOMEM;
		}
	} else {
		item->pkgname = NULL;
	}

	if (filename) {
		item->filename = strdup(filename);
		if (!item->filename) {
			free(item->pkgname);
			free(item);
			return -ENOMEM;
		}
	} else {
		item->filename = NULL;
	}

	item->funcname = strdup(cmd);
	if (!item->funcname) {
		free(item->filename);
		free(item->pkgname);
		free(item);
		return -ENOMEM;
	}
	item->param = param;
	item->slave = slave;

	item->ret_cb = ret_cb;
	item->cbdata = data;

	slave->sending_list = eina_list_append(slave->sending_list, item);
	check_and_fire_cmd_consumer(slave);

	return 0;
}

int slave_destroy(struct slave_node *slave)
{
	DbgPrint(">>>>>>>>>>>>> Slave %s [%d] is removed from the list\n", slave->name, slave->pid);
	s_info.slave_list = eina_list_remove(s_info.slave_list, slave);
	pkgmgr_delete_by_slave(slave);
	destroy_slave_data(slave);
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

int slave_get_fault_count(struct slave_node *slave)
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
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(s_info.slave_list, l, n, slave) {
		s_info.slave_list = eina_list_remove_list(s_info.slave_list, l);
		destroy_slave_data(slave);
	}

	return 0;
}

void slave_ping(struct slave_node *slave)
{
	if (slave->pong_timer)
		ecore_timer_reset(slave->pong_timer);
}

void slave_bye_bye(struct slave_node *slave)
{
	/*!
	 * \note
	 * Just reset the PID.
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
