#include <stdio.h>
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */
#include <libgen.h> /* basename */

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>

#include <gio/gio.h> /* GDBusProxy */

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "package.h"
#include "fault_manager.h"
#include "util.h"
#include "conf.h"

struct slave_rpc {
	Ecore_Timer *pong_timer;
	GDBusProxy *proxy;

	Eina_List *pending_request_list;

	unsigned long ping_count;
	unsigned long next_ping_count;
};

struct packet {
	/* create_packet, destroy_packet will care these varaibles */
	char *pkgname;
	char *filename;
	char *cmd;
	GVariant *param;
	struct slave_node *slave;

	/* Don't need to care these data */
	void (*ret_cb)(struct slave_node *slave, const char *funcname, GVariant *result, void *cbdata);
	void *cbdata;
};

static struct info {
	Eina_List *packet_list;
	Ecore_Timer *packet_consuming_timer;
} s_info = {
	.packet_list = NULL,
	.packet_consuming_timer = NULL,
};

static inline struct packet *create_packet(struct slave_node *slave, const char *pkgname, const char *filename, const char *cmd, GVariant *param)
{
	struct packet *packet;

	packet = calloc(1, sizeof(*packet));
	if (!packet) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	if (pkgname) {
		packet->pkgname = strdup(pkgname);
		if (!packet->pkgname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(packet);
			return NULL;
		}
	}

	if (filename) {
		packet->filename = strdup(filename);
		if (!packet->filename) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(packet->pkgname);
			free(packet);
			return NULL;
		}
	}

	if (cmd) {
		packet->cmd = strdup(cmd);
		if (!packet->cmd) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(packet->pkgname);
			free(packet->filename);
			free(packet);
			return NULL;
		}
	}

	packet->slave = slave_ref(slave); /*!< To prevent from destroying of the slave while communicating with the slave */
	packet->param = g_variant_ref(param);

	return packet;
}

static inline void destroy_packet(struct packet *packet)
{
	slave_unref(packet->slave);
	g_variant_unref(packet->param);
	free(packet->pkgname);
	free(packet->filename);
	free(packet->cmd);
	free(packet);
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

static void slave_async_cb(GDBusProxy *proxy, GAsyncResult *result, void *data)
{
	struct packet *packet;
	GError *err;
	GVariant *param;

	packet = data;

	if (!packet) {
		ErrPrint("Packet is NIL\n");
		return;
	}

	/*!
	 * \note
	 * packet->param is not valid from here.
	 */
	if (!slave_is_activated(packet->slave)) {
		ErrPrint("Slave is not activated (accidently dead)\n");
		if (packet->ret_cb)
			packet->ret_cb(packet->slave, packet->cmd, NULL, packet->cbdata);
		goto out;
	}

	err = NULL;
	param = g_dbus_proxy_call_finish(proxy, result, &err);
	if (!param) {
		char *cmd;
		//struct slave_rpc *rpc;

		cmd = packet->cmd ? packet->cmd : "";

		if (err) {
			ErrPrint("package[%s], cmd[%s]: %s\n", packet->pkgname, cmd, err->message);
			g_error_free(err);
		}

		if (packet->ret_cb)
			packet->ret_cb(packet->slave, cmd, NULL, packet->cbdata);

		#if 0
		char *filename;
		if (!fault_is_occured() && packet->pkgname) {
			/*!
			 * \note
			 * Recording the fault information.
			 * slave cannot detect its crash if a livebox
			 * uses callback, not only update_content but also other functions.
			 * To fix that case, mark the fault again from here.
			 */
			filename = packet->filename ? packet->filename : "";
			fault_func_call(packet->slave, packet->pkgname, filename, cmd);
		}
		#endif

		/*!
		 * \note
		 * Something is goging wrong.
		 * Try to deactivate this.
		 * And it will raise up the dead signal.
		 * then the dead signal callback will check the fault package.
		 * So we don't need to check the fault package from here.
		 */
		//rpc = slave_data(packet->slave, "rpc");
		//if (rpc && rpc->proxy == proxy)
		slave_faulted(packet->slave);

		goto out;
	}

	if (packet->ret_cb)
		packet->ret_cb(packet->slave, packet->cmd, param, packet->cbdata);
	else
		g_variant_unref(param);

out:
	destroy_packet(packet);
}

static Eina_Bool packet_consumer_cb(void *data)
{
	struct packet *packet;
	struct pkg_info *info = NULL;

	packet = pop_packet();
	if (!packet) {
		s_info.packet_consuming_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	if (packet->pkgname)
		info = package_find(packet->pkgname);

	if ((info && package_is_fault(info)) || !slave_is_activated(packet->slave)) {
		if (packet->ret_cb)
			packet->ret_cb(packet->slave, packet->cmd, NULL, packet->cbdata);
		destroy_packet(packet);
	} else {
		struct slave_rpc *rpc;

		rpc = slave_data(packet->slave, "rpc");
		if (!rpc) {
			ErrPrint("Slave has no rpc info\n");
			if (packet->ret_cb)
				packet->ret_cb(packet->slave, packet->cmd, NULL, packet->cbdata);
			destroy_packet(packet);
			return ECORE_CALLBACK_RENEW;
		}

		g_dbus_proxy_call(rpc->proxy, packet->cmd, packet->param,
				G_DBUS_CALL_FLAGS_NO_AUTO_START,
				-1, NULL, (GAsyncReadyCallback)slave_async_cb, packet);
	}

	return ECORE_CALLBACK_RENEW;
}

static inline void push_packet(struct packet *packet)
{
	s_info.packet_list = eina_list_append(s_info.packet_list, packet);

	if (s_info.packet_consuming_timer)
		return;

	s_info.packet_consuming_timer = ecore_timer_add(PACKET_TIME, packet_consumer_cb, NULL);
	if (!s_info.packet_consuming_timer) {
		ErrPrint("Failed to add packet consumer\n");
		s_info.packet_list = eina_list_remove(s_info.packet_list, packet);
		destroy_packet(packet);
	}
}

static int slave_deactivate_cb(struct slave_node *slave, void *data)
{
	struct slave_rpc *rpc;
	struct packet *packet;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		/*!
		 * \note
		 * Return negative value will remove this callback from the event list of the slave
		 */
		return -EINVAL;
	}

	if (rpc->proxy) {
		Eina_List *l;
		Eina_List *n;

		g_object_unref(rpc->proxy);
		rpc->proxy = NULL;

		if (rpc->pong_timer)
			ecore_timer_del(rpc->pong_timer);
		else
			ErrPrint("slave has no pong timer\n");

		EINA_LIST_FOREACH_SAFE(s_info.packet_list, l, n, packet) {
			if (packet->slave == slave) {
				s_info.packet_list = eina_list_remove(s_info.packet_list, packet);
				destroy_packet(packet);
			}
		}
	} else {
		EINA_LIST_FREE(rpc->pending_request_list, packet) {
			destroy_packet(packet);
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

	if (rpc->proxy) {
		g_object_unref(rpc->proxy);

		if (rpc->pong_timer)
			ecore_timer_del(rpc->pong_timer);
		else
			ErrPrint("Pong timer is not exists\n");
	} else {
		struct packet *packet;
		EINA_LIST_FREE(rpc->pending_request_list, packet) {
			destroy_packet(packet);
		}
	}

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

int slave_rpc_async_request(struct slave_node *slave,
			const char *pkgname, const char *filename,
			const char *cmd, GVariant *param, void (*ret_cb)(struct slave_node *slave, const char *func, GVariant *result, void *data),
			void *data)
{
	struct packet *packet;
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("RPC info is not valid\n");
		if (ret_cb)
			ret_cb(slave, cmd, NULL, data);

		g_variant_unref(param);
		return -EINVAL;
	}

	packet = create_packet(slave, pkgname, filename, cmd, param);
	if (!packet) {
		if (ret_cb)
			ret_cb(slave, cmd, NULL, data);

		g_variant_unref(param);
		return -ENOMEM;
	}

	packet->ret_cb = ret_cb;
	packet->cbdata = data;

	if (!rpc->proxy)
		rpc->pending_request_list = eina_list_append(rpc->pending_request_list, packet);
	else
		push_packet(packet);

	return 0;
}

int slave_rpc_sync_request(struct slave_node *slave,
			const char *pkgname, const char *filename,
			const char *cmd, GVariant *param)
{
	GVariant *result;
	GError *err = NULL;
	struct slave_rpc *rpc;

	if (!slave_is_activated(slave)) {
		g_variant_unref(param);
		return -EINVAL;
	}

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave has no rpc info\n");
		g_variant_unref(param);
		return -EINVAL;
	}

	if (!rpc->proxy) {
		ErrPrint("Slave is not ready to talk with master\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	result = g_dbus_proxy_call_sync(rpc->proxy, cmd, param,
				G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, &err);
	if (!result) {
		cmd = cmd ? cmd : "";

		if (err) {
			ErrPrint("Package[%s], cmd[%s]: %s\n", pkgname, cmd, err->message);
			g_error_free(err);
		}

		if (!fault_is_occured() && pkgname) {
			/*!
			 * \note
			 * Recording the fault information.
			 * slave cannot detect its crash if a livebox
			 * uses callback, not only update_content but also other functions.
			 * To fix that case, mark the fault again from here.
			 */
			filename = filename ? filename : "";
			fault_func_call(slave, pkgname, filename, cmd);
		}

		/*!
		 * \note
		 *
		 * Something is goging wrong.
		 * Try to deactivate this.
		 * And it will raise up the dead signal.
		 * then the dead signal callback will check the fault package.
		 * So we don't need to check the fault package from here.
		 */
		slave_faulted(slave);
		ErrPrint("Failed to send sync request: %s\n", cmd);
		return -EIO;
	}

	g_variant_unref(result);
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

	rpc->proxy = NULL;

	if (slave_set_data(slave, "rpc", rpc) < 0) {
		free(rpc);
		return -ENOMEM;
	}

	slave_event_callback_add(slave, SLAVE_EVENT_DELETE, slave_del_cb, NULL);
	slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb, NULL);
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

int slave_rpc_reset_proxy(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Failed to get RPC info\n");
		return -EINVAL;
	}

	rpc->proxy = NULL;
	return 0;
}

int slave_rpc_update_proxy(struct slave_node *slave, GDBusProxy *proxy)
{
	struct slave_rpc *rpc;
	struct packet *packet;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Failed to get RPC info\n");
		return -EINVAL;
	}

	if (rpc->proxy)
		ErrPrint("proxy %p is already exists (%s %d) (replaced with %p)\n", rpc->proxy, slave_name(slave), slave_pid(slave), proxy);
	else
		ErrPrint("proxy %p is updated (%s %d)\n", proxy, slave_name(slave), slave_pid(slave));

	rpc->proxy = proxy;

	EINA_LIST_FREE(rpc->pending_request_list, packet) {
		push_packet(packet);
	}

	rpc->ping_count = 0;
	rpc->next_ping_count = 1;

	if (rpc->pong_timer)
		ecore_timer_del(rpc->pong_timer);

	rpc->pong_timer = ecore_timer_add(g_conf.ping_time, ping_timeout_cb, slave);
	if (!rpc->pong_timer)
		ErrPrint("Failed to add ping timer\n");

	slave_activated(slave);

	slave_reset_fault(slave);
	return 0;
}

void slave_rpc_request_update(const char *pkgname, const char *cluster, const char *category)
{
	struct slave_node *slave;
	struct pkg_info *info;
	GVariant *param;

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

	param = g_variant_new("(sss)", pkgname, cluster, category);
	if (!param) {
		ErrPrint("Failed to create a new param\n");
		return;
	}

	(void)slave_rpc_async_request(slave, pkgname, NULL, "update_content", param, NULL, NULL);
}

/* End of a file */
