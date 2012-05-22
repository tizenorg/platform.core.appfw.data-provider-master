#include <stdio.h>
#include <errno.h>
#include <stdlib.h> /* free */
#include <string.h> /* strcmp */
#include <libgen.h> /* basename */

#include <Evas.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <dlog.h>

#include "debug.h"
#include "pkg_manager.h"
#include "fault_manager.h"
#include "slave_manager.h"
#include "client_manager.h"
#include "script_handler.h"
#include "util.h"
#include "conf.h"
#include "rpc_to_slave.h"
#include "ctx_client.h"

static struct info {
	GDBusNodeInfo *node_info;
	guint owner_id;
	guint reg_id;
	const gchar *xml_data;
} s_info = {
	.node_info = NULL,
	.owner_id = 0,
	.xml_data = "<node name ='" OBJECT_PATH "'>"
	"<interface name='" SERVICE_INTERFACE "'>"

	/* From client */
	" <method name='acquire'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='release'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='clicked'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='s' name='event' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='text_signal'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='s' name='emission' direction='in' />"
	"  <arg type='s' name='source' direction='in' />"
	"  <arg type='d' name='sx' direction='in' />"
	"  <arg type='d' name='sy' direction='in' />"
	"  <arg type='d' name='ex' direction='in' />"
	"  <arg type='d' name='ey' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='delete'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='resize'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='w' direction='in' />"
	"  <arg type='i' name='h' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='new'>"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='content' direction='in' />"
	"  <arg type='s' name='cluster' direction='in' />"
	"  <arg type='s' name='category' direction='in' />"
	"  <arg type='d' name='period' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='set_period'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='d' name='period' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='change_group'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='s' name='cluster' direction='in' />"
	"  <arg type='s' name='category' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pd_mouse_down'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pd_mouse_up'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pd_mouse_move'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='lb_mouse_move'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='lb_mouse_down'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='lb_mouse_up'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pinup_changed'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='pinup' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='create_pd'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='destroy_pd'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='activate_package'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='livebox_is_exists'>"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"

	/* From slave */
	" <method name='ping'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='call'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='s' name='funcname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='ret'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='s' name='funcname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='hello'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='bye'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='updated'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='priority' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='desc_updated'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='s' name='descfile' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='deleted'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='filename' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"

	"</interface>"
	"</node>",
};

static void method_ping(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	struct slave_node *node;
	int ret;

	g_variant_get(param, "(&s)", &slavename);

	node = slave_find(slavename);
	if (!node) {
		ErrPrint("Unknown slave! %s\n", slavename);
		ret = -EINVAL;
	} else {
		slave_ping(node);
		ret = 0;
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_call(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	const char *funcname;
	const char *slave_name;
	struct slave_node *node;
	int ret;

	g_variant_get(param, "(&s&s&s&s)", &slave_name, &pkgname, &filename, &funcname);

	node = slave_find(slave_name);
	if (!node) {
		ErrPrint("Failed to find a correct slave: %s\n", slave_name);
		ret = -EFAULT;
	} else {
		ret = fault_func_call(node, pkgname, filename, funcname);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_ret(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	const char *funcname;
	const char *slave_name;
	struct slave_node *node;
	int ret;

	g_variant_get(param, "(&s&s&s&s)", &slave_name, &pkgname, &filename, &funcname);

	node = slave_find(slave_name);
	if (!node) {
		ErrPrint("Failed to find a correct slave: %s\n", slave_name);
		ret = -EFAULT;
	} else {
		ret = fault_func_ret(node, pkgname, filename, funcname);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_text_signal(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	const char *emission;
	const char *source;
	double sx;
	double sy;
	double ex;
	double ey;
	struct client_node *client;
	GDBusConnection *conn;
	int ret;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		/*! NOTE: Ignoring this and
		 *        blocking the main loop of the client if it waiting this result!
		 */
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		/*! NOTE: Ignoring this and
		 *        blocking the main loop of the client if it waiting this result!
		 */
		return;
	}

	g_variant_get(param, "(&s&s&s&sdddd)", &pkgname, &filename, &emission, &source, &sx, &sy, &ex, &ey);

	if (pkgmgr_is_fault(pkgname)) {
		ret = -EAGAIN;
	} else {
		struct slave_node *slave;

		slave = pkgmgr_slave(pkgname);
		if (!slave) {
			ErrPrint("Package[%s - %s] is not loaded\n", pkgname, filename);
			ret = -ENETUNREACH;
		} else {
			/*!
			 * \NOTE: just reuse "param" or renew it?
			 *        Now, I'll renew it.
			 *        Please, don't ask me why.
			 */
			param = g_variant_new("(ssssdddd)", pkgname, filename, emission, source, sx, sy, ex, ey);
			if (!param)
				ret = -EFAULT;
			else
				ret = slave_push_command(slave, pkgname, filename, "text_signal", param, NULL, NULL);
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a varian\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_clicked(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	const char *event;
	double timestamp;
	double x;
	double y;
	int ret;
	struct client_node *client;
	GDBusConnection *conn;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		return;
	}

	g_variant_get(param, "(&s&s&sddd)", &pkgname, &filename, &event, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/* TODO: Load this package */
		ret = -EAGAIN;
	} else {
		struct slave_node *slave;

		slave = pkgmgr_slave(pkgname);
		if (!slave) {
			/* TODO: Impossible */
			ErrPrint("Package[%s - %s] is not loaded\n", pkgname, filename);
			ret = -ENETUNREACH;
		} else {
			/* NOTE: param is resued from here */
			param = g_variant_new("(sssddd)", pkgname, filename, event, timestamp, x, y);
			if (!param) {
				ret = -EFAULT;
			} else {
				ret = slave_push_command(slave,
						pkgname, filename,
						"clicked", param, NULL, NULL);
			}
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void on_slave_signal(GDBusProxy *proxy, gchar *sender, gchar *signame, GVariant *param, gpointer data)
{
	DbgPrint("Sender: %s\n", sender);
	DbgPrint("Signame: %s\n", signame);
}

static void slave_proxy_prepared_cb(GObject *obj, GAsyncResult *res, gpointer slave)
{
	GDBusProxy *proxy;
	GError *err;
	int ret;

	err = NULL;
	proxy = g_dbus_proxy_new_finish(res, &err);
	if (!proxy) {
		if (err) {
			ErrPrint("Proxy new: %s\n", err->message);
			g_error_free(err);
		}
		return;
	}

	g_signal_connect(proxy, "g-signal", G_CALLBACK(on_slave_signal), NULL);

	ret = slave_update_proxy(slave, proxy);
	if (ret < 0)
		g_object_unref(proxy);
}

static void method_hello(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	struct slave_node *slave;
	GDBusConnection *conn;

	g_variant_get(param, "(&s)", &slavename);

	slave = slave_find(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);

		param = g_variant_new("(i)", -EINVAL);
		if (!param)
			ErrPrint("Failed to create variant\n");

		g_dbus_method_invocation_return_value(inv, param);
	} else {
		const char *sender;

		sender = g_dbus_method_invocation_get_sender(inv);

		param = g_variant_new("(i)", 0);
		if (!param)
			ErrPrint("Failed to create variant\n");

		g_dbus_method_invocation_return_value(inv, param);

		conn = g_dbus_method_invocation_get_connection(inv);
		if (!conn) {
			ErrPrint("Failed to get connection object\n");
			return;
		}

		g_dbus_proxy_new(conn,
			G_DBUS_PROXY_FLAGS_NONE,
			NULL, 
			sender,
			OBJECT_PATH,
			SERVICE_INTERFACE,
			NULL,
			slave_proxy_prepared_cb, slave);
	}
}

static void method_bye(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s)", &slavename);

	slave = slave_find(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		/*!
		 * \note
		 * Update the PID value
		 * for prevent trying process temination
		 * from destroyer
		 */
		slave_bye_bye(slave);
		slave_destroy(slave);
		ret = 0;
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_desc_updated(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	const char *pkgname;
	const char *filename;
	const char *descfile;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s&s&s&s)", &slavename, &pkgname, &filename, &descfile);

	slave = slave_find(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		struct inst_info *inst;

		inst = pkgmgr_find(pkgname, filename);
		if (inst) {
			if (pkgmgr_text_pd(inst))
				ret = pkgmgr_pd_updated(pkgname, filename, descfile, 0, 0);
			else
				ret = script_handler_parse_desc(pkgname, filename, descfile, 1);
		} else {
			ret = -EINVAL;
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_updated(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	const char *pkgname;
	const char *filename;
	int x;
	int y;
	double priority;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s&s&siid)", &slavename, &pkgname, &filename, &x, &y, &priority);

	slave = slave_find(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		struct inst_info *inst;

		inst = pkgmgr_find(pkgname, filename);
		if (inst) {
			if (pkgmgr_lb_script(inst))
				ret = script_handler_parse_desc(pkgname, filename, filename, 0);
			else /* pkgmgr_text_lb(inst) */
				ret = pkgmgr_lb_updated(pkgname, filename, x, y, priority);
		} else {
			ret = -EINVAL;
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_deleted(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	const char *pkgname;
	const char *filename;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s&s&s)", &slavename, &pkgname, &filename);

	slave = slave_find(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		DbgPrint("Package %s is deleted\n", pkgname);
		ret = pkgmgr_deleted(pkgname, filename);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void on_client_signal(GDBusProxy *proxy, gchar *sender, gchar *signame, GVariant *param, gpointer data)
{
	DbgPrint("Sender: %s\n", sender);
	DbgPrint("SigName: %s\n", signame);
}

static void client_proxy_prepared_cb(GObject *obj, GAsyncResult *res, gpointer client)
{
	GDBusProxy *proxy;
	GError *err;
	int ret;

	err = NULL;
	proxy = g_dbus_proxy_new_finish(res, &err);
	if (!proxy) {
		if (err) {
			ErrPrint("Proxy new: %s\n", err->message);
			g_error_free(err);
		}

		return;
	}

	g_signal_connect(proxy, "g-signal", G_CALLBACK(on_client_signal), NULL);

	ret = client_update_proxy(client, proxy);
	if (ret < 0)
		g_object_unref(proxy);
}

static void method_acquire(GDBusMethodInvocation *inv, GVariant *param)
{
	struct client_node *client;
	int client_pid;
	int ret;

	g_variant_get(param, "(i)", &client_pid);

	ret = 0;

	client = client_find(client_pid);
	if (client) {
		ErrPrint("%d is already registered client\n", client_pid);
		ret = -EEXIST;
		goto errout;
	}

	client = client_new(client_pid);
	if (!client) {
		ErrPrint("Failed to create client: %d\n", client_pid);
		ret = -EFAULT;
		goto errout;
	}

	DbgPrint("Client %d is created\n", client_pid);

errout:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);

	if (ret == 0) {
		GDBusConnection *conn;
		const char *sender;

		conn = g_dbus_method_invocation_get_connection(inv);
		if (!conn) {
			ErrPrint("Failed to get connection object\n");
			return;
		}

		sender = g_dbus_method_invocation_get_sender(inv);
		g_dbus_proxy_new(conn,
			G_DBUS_PROXY_FLAGS_NONE,
			NULL,
			sender,
			OBJECT_PATH,
			SERVICE_INTERFACE,
			NULL,
			client_proxy_prepared_cb, client);
	}
}

static void method_release(GDBusMethodInvocation *inv, GVariant *param)
{
	struct client_node *client;
	int pid;
	int ret;

	g_variant_get(param, "(i)", &pid);

	client = client_find(pid);
	if (!client) {
		ErrPrint("Unknown client: %d\n", pid);
		ret = -EINVAL;
	} else {
		client_destroy(client);
		ret = 0;
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_lb_mouse_up(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;

	g_variant_get(param, "(ssiiddd)", &pkgname, &filename, &w, &h, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		struct script_info *info;
		Evas *e;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst) {
			ret = -ENOENT;
			goto out;
		}

		info = pkgmgr_lb_script(inst);
		if (!info) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(info);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}


		evas_event_feed_mouse_up(e, 1, EVAS_BUTTON_NONE, timestamp, NULL);
		evas_event_feed_mouse_out(e, timestamp, NULL);
		ret = 0;
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_create_pd(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	struct inst_info *inst;
	int ret;

	g_variant_get(param, "(&s&s)", &pkgname, &filename);

	inst = pkgmgr_find(pkgname, filename);
	if (!inst)
		ret = -ENOENT;
	else
		ret = pkgmgr_load_pd(inst);

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);

}

static void method_livebox_is_exists(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	int ret;

	g_variant_get(param, "(&s)", &pkgname);

	ret = util_validate_livebox_package(pkgname);

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_activate_pkg(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	int ret;

	g_variant_get(param, "(&s)", &pkgname);

	ret = pkgmgr_clear_fault(pkgname);

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_destroy_pd(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	struct inst_info *inst;
	int ret;

	g_variant_get(param, "(&s&s)", &pkgname, &filename);

	inst = pkgmgr_find(pkgname, filename);
	if (!inst)
		ret = -ENOENT;
	else
		ret = pkgmgr_unload_pd(inst);

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_pinup_changed(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int pinup;
	int ret;

	g_variant_get(param, "(&s&si)", &pkgname, &filename, &pinup);

	if (pkgmgr_is_fault(pkgname)) {
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst)
			ret = -ENOENT;
		else
			ret = pkgmgr_set_pinup(inst, pinup);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_lb_mouse_down(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;

	g_variant_get(param, "(&s&siiddd)", &pkgname, &filename, &w, &h, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		struct script_info *info;
		Evas *e;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst) {
			ret = -ENOENT;
			goto out;
		}

		info = pkgmgr_lb_script(inst);
		if (!info) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(info);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		evas_event_feed_mouse_in(e, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp, NULL);
		ret = 0;
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_lb_mouse_move(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;

	g_variant_get(param, "(&s&siiddd)", &pkgname, &filename, &w, &h, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		struct script_info *info;
		Evas *e;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst) {
			ret = -ENOENT;
			goto out;
		}

		info = pkgmgr_lb_script(inst);
		if (!info) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(info);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		evas_event_feed_mouse_move(e, w * x, h * y, timestamp, NULL);
		ret = 0;
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_pd_mouse_move(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;

	g_variant_get(param, "(&s&siiddd)", &pkgname, &filename, &w, &h, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		struct script_info *info;
		Evas *e;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst) {
			ret = -ENOENT;
			goto out;
		}

		info = pkgmgr_pd_script(inst);
		if (!inst) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(info);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		ret = 0;
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_pd_mouse_up(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;

	g_variant_get(param, "(&s&siiddd)", &pkgname, &filename, &w, &h, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		struct script_info *info;
		Evas *e;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst) {
			ret = -ENOENT;
			goto out;
		}

		info = pkgmgr_pd_script(inst);
		if (!inst) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(info);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		evas_event_feed_mouse_up(e, 1, EVAS_BUTTON_NONE, timestamp, NULL);
		evas_event_feed_mouse_out(e, timestamp, NULL);
		ret = 0;
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_pd_mouse_down(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;

	g_variant_get(param, "(&s&siiddd)", &pkgname, &filename, &w, &h, &timestamp, &x, &y);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		struct script_info *info;
		Evas *e;

		inst = pkgmgr_find(pkgname, filename);
		if (!inst) {
			ret = -ENOENT;
			goto out;
		}

		info = pkgmgr_pd_script(inst);
		if (!inst) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(info);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		evas_event_feed_mouse_in(e, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp, NULL);
		ret = 0;
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_change_group(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	const char *cluster;
	const char *category;
	int ret;
	struct client_node *client;
	GDBusConnection *conn;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		return;
	}

	g_variant_get(param, "(&s&s&s&s)", &pkgname, &filename, &cluster, &category);

	if (pkgmgr_is_fault(pkgname)) {
		ret = -EAGAIN;
	} else {
		struct slave_node *slave;

		slave = pkgmgr_slave(pkgname);
		if (!slave) {
			ErrPrint("Package[%s] is not loaed\n", pkgname);
			ret = -ENETUNREACH;
		} else {
			param = g_variant_new("(ssss)", pkgname, filename, cluster, category);
			if (!param)
				ret = -EFAULT;
			else
				ret = slave_push_command(slave, pkgname, filename, "change_group", param, NULL, NULL);
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_delete(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int ret;
	struct client_node *client;
	GDBusConnection *conn;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		return;
	}

	g_variant_get(param, "(&s&s)", &pkgname, &filename);

	if (pkgmgr_is_fault(pkgname)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct slave_node *slave;

		slave = pkgmgr_slave(pkgname);
		if (!slave) {
			ErrPrint("Package[%s - %s] is not loaded\n", pkgname, filename);
			ret = -ENETUNREACH;
		} else {
			/* NOTE: param is resued from here */
			param = g_variant_new("(ss)", pkgname, filename);
			if (!param)
				ret = -EFAULT;
			else
				ret = slave_push_command(slave, pkgname, filename, "delete", param, NULL, NULL);
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_resize(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	int w;
	int h;
	int ret;
	struct client_node *client;
	GDBusConnection *conn;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		return;
	}

	g_variant_get(param, "(&s&sii)", &pkgname, &filename, &w, &h);

	if (pkgmgr_is_fault(pkgname)) {
		/* TODO: Load this package */
		ret = -EAGAIN;
	} else {
		struct slave_node *slave;

		slave = pkgmgr_slave(pkgname);
		if (!slave) {
			/* TODO: Impossible */
			ErrPrint("Package[%s - %s] is not loaded\n", pkgname, filename);
			ret = -ENETUNREACH;
		} else {
			/* NOTE: param is resued from here */
			param = g_variant_new("(ssii)", pkgname, filename, w, h);
			if (!param)
				ret = -EFAULT;
			else
				ret = slave_push_command(slave, pkgname, filename, "resize", param, NULL, NULL);
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static inline void validate_period(double *period)
{
	/* TODO:
	 * Check the period value, is it valid?
	 */
	if (*period < 0.0f)
		*period = 0.0f;
}

static void method_set_period(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *filename;
	double period;
	int ret;
	GDBusConnection *conn;
	struct client_node *client;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		return;
	}

	g_variant_get(param, "(&s&sd)", &pkgname, &filename, &period);

	validate_period(&period);

	if (util_validate_livebox_package(pkgname) < 0) {
		ret = -EINVAL;
	} else if (pkgmgr_is_fault(pkgname)) {
		ret = -EAGAIN;
	} else {
		struct slave_node *slave;

		slave = pkgmgr_slave(pkgname);
		if (!slave) {
			ErrPrint("Package[%s - %s] is not loaded\n", pkgname, filename);
			ret = -ENETUNREACH;
		} else {
			param = g_variant_new("(ssd)", pkgname, filename, period);
			if (!param)
				ret = -EFAULT;
			else
				ret = slave_push_command(slave, pkgname, filename, "set_period", param, NULL, NULL);
		}
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to make a return variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_new(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *content;
	const char *cluster;
	const char *category;
	double period;
	int ret;
	double timestamp;
	struct client_node *client;
	GDBusConnection *conn;

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection\n");
		return;
	}

	client = client_find_by_connection(conn);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		return;
	}

	g_variant_get(param, "(d&s&s&s&sd)", &timestamp, &pkgname, &content, &cluster, &category, &period);

	if (util_validate_livebox_package(pkgname) < 0) {
		ret = -EINVAL;
	} else if (pkgmgr_is_fault(pkgname)) {
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;
		inst = rpc_send_create_request(client, pkgname, content, cluster, category, timestamp, period);
		ret = inst ? 0 : -EFAULT;
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_handler(GDBusConnection *conn,
		const gchar *sender,
		const gchar *object_path,
		const gchar *iface_name,
		const gchar *method,
		GVariant *param,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	register int i;
	struct method_table {
		const char *name;
		void (*method)(GDBusMethodInvocation *, GVariant *);
	} method_table[] = {
		/* For viewer */
		{
			.name = "clicked",
			.method = method_clicked,
		},
		{
			.name = "text_signal",
			.method = method_text_signal,
		},
		{
			.name = "resize",
			.method = method_resize,
		},
		{
			.name = "new",
			.method = method_new,
		},
		{
			.name = "set_period",
			.method = method_set_period,
		},
		{
			.name = "delete",
			.method = method_delete,
		},
		{
			.name = "change_group",
			.method = method_change_group,
		},
		{
			.name = "pd_mouse_down",
			.method = method_pd_mouse_down,
		},
		{
			.name = "pd_mouse_up",
			.method = method_pd_mouse_up,
		},
		{
			.name = "pd_mouse_move",
			.method = method_pd_mouse_move,
		},
		{
			.name = "lb_mouse_down",
			.method = method_lb_mouse_down,
		},
		{
			.name = "lb_mouse_move",
			.method = method_lb_mouse_move,
		},
		{
			.name = "lb_mouse_up",
			.method = method_lb_mouse_up,
		},
		{
			.name = "pinup_changed",
			.method = method_pinup_changed,
		},
		{
			.name = "create_pd",
			.method = method_create_pd,
		},
		{
			.name = "destroy_pd",
			.method = method_destroy_pd,
		},
		{
			.name = "activate_package",
			.method = method_activate_pkg,
		},
		{
			.name = "livebox_is_exists",
			.method = method_livebox_is_exists,
		},

		/* For slave */
		{
			.name = "ping",
			.method = method_ping,
		},
		{
			.name = "call",
			.method = method_call,
		},
		{
			.name = "ret",
			.method = method_ret,
		},
		{
			.name = "hello",
			.method = method_hello,
		},
		{
			.name = "bye",
			.method = method_bye,
		},
		{
			.name = "updated",
			.method = method_updated,
		},
		{
			.name = "desc_updated",
			.method = method_desc_updated,
		},
		{
			.name = "deleted",
			.method = method_deleted,
		},

		/* For client */
		{
			.name = "acquire",
			.method = method_acquire,
		},
		{
			.name = "release",
			.method = method_release,
		},
		{
			.name = NULL,
			.method = NULL,
		},
	};

	for (i = 0; method_table[i].name; i++) {
		if (!g_strcmp0(method, method_table[i].name) && method_table[i].method) {
			method_table[i].method(invocation, param);
			break;
		}
	}
}

static const GDBusInterfaceVTable iface_vtable = {
	method_handler,
	NULL,
	NULL,
};

static void on_bus_acquired(GDBusConnection *conn,
		const char *name,
		gpointer user_data)
{
	GError *err;

	err = NULL;
	s_info.node_info = g_dbus_node_info_new_for_xml(s_info.xml_data, &err);
	if (!s_info.node_info) {
		if (err) {
			ErrPrint("error - %s\n", err->message);
			g_error_free(err);
		}

		return;
	}

	err = NULL;
	s_info.reg_id = g_dbus_connection_register_object(conn,
			OBJECT_PATH,
			s_info.node_info->interfaces[0],
			&iface_vtable,
			NULL,
			NULL,
			&err);

	if (s_info.reg_id <= 0) {
		if (err) {
			ErrPrint("register %s - %s\n", OBJECT_PATH, err->message);
			g_error_free(err);
		}
		g_dbus_node_info_unref(s_info.node_info);
		s_info.node_info = NULL;
	}
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	DbgPrint("%s\n", name);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	DbgPrint("%s\n", name);
}

int dbus_init(void)
{
	int r;

	r = g_bus_own_name(BUS_TYPE,
			SERVICE_NAME,
			G_BUS_NAME_OWNER_FLAGS_NONE,
			on_bus_acquired,
			on_name_acquired,
			on_name_lost,
			NULL, /* user_data */
			NULL /* GDestroyNotify */ );
	if (r <= 0) {
		ErrPrint("Failed to get a name: %s\n", SERVICE_NAME);
		return -EFAULT;
	}

	s_info.owner_id = r;
	return 0;
}

int dbus_fini(void)
{
	if (s_info.node_info) {
		g_dbus_node_info_unref(s_info.node_info);
		s_info.node_info = NULL;
	}

	if (s_info.owner_id) {
		g_bus_unown_name(s_info.owner_id);
		s_info.owner_id = 0;
	}

	return 0;
}

/* End of a file */
