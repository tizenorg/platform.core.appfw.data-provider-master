#include <stdio.h>
#include <stdlib.h> /* free */
#include <string.h> /* strcmp */
#include <libgen.h> /* basename */

#include <Evas.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <dlog.h>

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "client_rpc.h"
#include "fault_manager.h"
#include "instance.h"
#include "script_handler.h"
#include "util.h"
#include "conf.h"
#include "rpc_to_slave.h"
#include "ctx_client.h"
#include "package.h"

static struct info {
	GDBusNodeInfo *node_info;
	guint owner_id;
	guint reg_id;
	const gchar *xml_data;
} s_info = {
	.node_info = NULL,
	.owner_id = 0,
	.xml_data = "<node name ='" MASTER_OBJECT_PATH "'>"
	"<interface name='" MASTER_SERVICE_INTERFACE "'>"

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
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='s' name='event' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='text_signal'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='s' name='emission' direction='in' />"
	"  <arg type='s' name='source' direction='in' />"
	"  <arg type='d' name='sx' direction='in' />"
	"  <arg type='d' name='sy' direction='in' />"
	"  <arg type='d' name='ex' direction='in' />"
	"  <arg type='d' name='ey' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='delete'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='resize'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='w' direction='in' />"
	"  <arg type='i' name='h' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='new'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='content' direction='in' />"
	"  <arg type='s' name='cluster' direction='in' />"
	"  <arg type='s' name='category' direction='in' />"
	"  <arg type='d' name='period' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='set_period'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='d' name='period' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='change_group'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='s' name='cluster' direction='in' />"
	"  <arg type='s' name='category' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pd_mouse_down'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pd_mouse_up'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pd_mouse_move'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='lb_mouse_move'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='lb_mouse_down'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='lb_mouse_up'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='timestamp' direction='in' />"
	"  <arg type='d' name='x' direction='in' />"
	"  <arg type='d' name='y' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='pinup_changed'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='pinup' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='create_pd'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='destroy_pd'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='activate_package'>"
	"  <arg type='i' name='client_id' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='livebox_is_exists'>"
	"  <arg type='i' name='client_id' direction='in' />"
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
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='s' name='funcname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='ret'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='s' name='funcname' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='hello'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='updated'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='width' direction='in' />"
	"  <arg type='i' name='height' direction='in' />"
	"  <arg type='d' name='priority' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='desc_updated'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='s' name='descfile' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"
	" <method name='deleted'>"
	"  <arg type='s' name='slave_name' direction='in' />"
	"  <arg type='s' name='pkgname' direction='in' />"
	"  <arg type='s' name='id' direction='in' />"
	"  <arg type='i' name='result' direction='out' />"
	" </method>"

	"</interface>"
	"</node>",
};

static void method_ping(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s)", &slavename);
	slave = slave_find_by_name(slavename);

	if (!slave) {
		ErrPrint("Unknown slave! %s\n", slavename);
		ret = -EINVAL;
	} else {
		ret = 0;
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);

	if (slave)
		slave_rpc_ping(slave);
}

static void method_call(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	const char *funcname;
	const char *slave_name;
	struct slave_node *node;
	int ret;

	g_variant_get(param, "(&s&s&s&s)", &slave_name, &pkgname, &id, &funcname);
	node = slave_find_by_name(slave_name);

	if (!node) {
		ErrPrint("Failed to find a correct slave: %s\n", slave_name);
		ret = -EFAULT;
	} else {
		ret = fault_func_call(node, pkgname, id, funcname);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_ret(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	const char *funcname;
	const char *slave_name;
	struct slave_node *node;
	int ret;

	g_variant_get(param, "(&s&s&s&s)", &slave_name, &pkgname, &id, &funcname);

	node = slave_find_by_name(slave_name);
	if (!node) {
		ErrPrint("Failed to find a correct slave: %s\n", slave_name);
		ret = -EFAULT;
	} else {
		ret = fault_func_ret(node, pkgname, id, funcname);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_text_signal(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	const char *emission;
	const char *source;
	double sx;
	double sy;
	double ex;
	double ey;
	struct client_node *client;
	int ret;
	pid_t pid;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&s&s&sdddd)", &pid, &pkgname, &id, &emission, &source, &sx, &sy, &ex, &ey);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a connected client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EAGAIN;
	else
		ret = instance_text_signal_emit(inst, emission, source, sx, sy, ex, ey);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a varian\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_clicked(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	const char *event;
	double timestamp;
	double x;
	double y;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&s&sddd)", &pid, &pkgname, &id, &event, &timestamp, &x, &y);

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_clicked(inst, event, timestamp, x, y);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void on_slave_signal(GDBusProxy *proxy, gchar *sender, gchar *signame, GVariant *param, gpointer data)
{
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

	ret = slave_rpc_update_proxy(slave, proxy);
	if (ret < 0)
		g_object_unref(proxy);
}

static void method_hello(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	const char *sender;
	struct slave_node *slave;
	GDBusConnection *conn;

	g_variant_get(param, "(&s)", &slavename);

	slave = slave_find_by_name(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);

		param = g_variant_new("(i)", -EINVAL);
		if (!param)
			ErrPrint("Failed to create variant\n");

		g_dbus_method_invocation_return_value(inv, param);
		return;
	} 

	sender = g_dbus_method_invocation_get_sender(inv);

	conn = g_dbus_method_invocation_get_connection(inv);
	if (!conn) {
		ErrPrint("Failed to get connection object\n");
		return;
	}

	g_dbus_proxy_new(conn,
		G_DBUS_PROXY_FLAGS_NONE,
		NULL, 
		sender,
		SLAVE_OBJECT_PATH,
		SLAVE_SERVICE_INTERFACE,
		NULL,
		slave_proxy_prepared_cb, slave);

	param = g_variant_new("(i)", 0);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_desc_updated(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *descfile;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s&s&s&s)", &slavename, &pkgname, &id, &descfile);

	slave = slave_find_by_name(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		struct inst_info *inst;

		inst = package_find_instance_by_id(pkgname, id);
		if (!inst) {
			ret = -ENOENT;
		} else if (package_is_fault(instance_package(inst))) {
			ErrPrint("Faulted package cannot make event\n");
			ret = -EAGAIN;
		} else if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Instance is already destroyed\n");
			ret = -EINVAL;
		} else if (package_pd_type(instance_package(inst)) == PD_TYPE_TEXT) {
			instance_set_pd_info(inst, 0, 0);
			instance_pd_updated(pkgname, id, descfile);
			ret = 0;
		} else if (script_handler_is_loaded(instance_pd_handle(inst))) {
			ret = script_handler_parse_desc(pkgname, id, descfile, 1);
		} else {
			ret = 0;
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
	const char *id;
	int w;
	int h;
	double priority;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s&s&siid)", &slavename, &pkgname, &id, &w, &h, &priority);

	slave = slave_find_by_name(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		struct inst_info *inst;

		inst = package_find_instance_by_id(pkgname, id);
		if (!inst) {
			ret = -ENOENT;
		} else if (package_is_fault(instance_package(inst))) {
			ErrPrint("Faulted instance cannot make any event.\n");
			ret = -EAGAIN;
		} else if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Instance is already destroyed\n");
			ret = -EINVAL;
		} else {
			instance_set_lb_info(inst, w, h, priority);

			if (package_lb_type(instance_package(inst)) == LB_TYPE_SCRIPT) {
				script_handler_resize(instance_lb_handle(inst), w, h);
				ret = script_handler_parse_desc(pkgname, id, id, 0);
			} else {
				/*!
				 * \check
				 * text format (inst)
				 */
				instance_lb_updated(pkgname, id);
				ret = 0;
			}
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
	const char *id;
	struct slave_node *slave;
	int ret;

	g_variant_get(param, "(&s&s&s)", &slavename, &pkgname, &id);

	slave = slave_find_by_name(slavename);
	if (!slave) {
		ErrPrint("Unknown slave: %s\n", slavename);
		ret = -EINVAL;
	} else {
		struct inst_info *inst;

		inst = package_find_instance_by_id(pkgname, id);
		if (!inst)
			ret = -ENOENT;
		else if (package_is_fault(instance_package(inst)))
			ret = -EAGAIN;
		else
			ret = instance_destroyed(inst);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void on_client_signal(GDBusProxy *proxy, gchar *sender, gchar *signame, GVariant *param, gpointer data)
{
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

		client_fault(client);
		return;
	}

	g_signal_connect(proxy, "g-signal", G_CALLBACK(on_client_signal), NULL);

	ret = client_rpc_update_proxy(client, proxy);
	if (ret < 0)
		g_object_unref(proxy);
}

static void method_acquire(GDBusMethodInvocation *inv, GVariant *param)
{
	pid_t pid;
	int ret;
	struct client_node *client;

	g_variant_get(param, "(i)", &pid);

	client = client_find_by_pid(pid);
	if (client) {
		ErrPrint("Client is already exists\n");
		ret = -EEXIST;
		goto out;
	}

	ret = 0;
	client = client_create(pid);
	if (!client) {
		ErrPrint("Failed to create client: %d\n", pid);
		ret = -EFAULT;
	}

out:
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
			CLIENT_OBJECT_PATH,
			CLIENT_SERVICE_INTERFACE,
			NULL,
			client_proxy_prepared_cb, client);
	}

	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);

}

static void method_release(GDBusMethodInvocation *inv, GVariant *param)
{
	int pid;
	struct client_node *client;

	g_variant_get(param, "(i)", &pid);

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		param = g_variant_new("(i)", -EINVAL);
	} else {
		param = g_variant_new("(i)", 0);
	}
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
	client_destroy(client);
}

static void method_lb_mouse_up(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&siiddd)", &pid, &pkgname, &id, &w, &h, &timestamp, &x, &y);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct script_info *script;
		Evas *e;

		script = instance_lb_handle(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		evas_event_feed_mouse_up(e, 1, EVAS_BUTTON_NONE, timestamp, NULL);
		evas_event_feed_mouse_out(e, timestamp + 0.01f, NULL);
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
	const char *id;
	struct inst_info *inst;
	int ret;
	pid_t pid;
	struct client_node *client;

	g_variant_get(param, "(i&s&s)", &pid, &pkgname, &id);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = script_handler_load(instance_pd_handle(inst), 1);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);

}

static void method_livebox_is_exists(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	int ret;
	pid_t pid;
	struct client_node *client;

	g_variant_get(param, "(i&s)", &pid, &pkgname);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	ret = util_validate_livebox_package(pkgname);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_activate_pkg(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct pkg_info *info;

	g_variant_get(param, "(i&s)", &pid, &pkgname);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	info = package_find(pkgname);
	if (!info)
		ret = -ENOENT;
	else
		ret = package_clear_fault(info);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_destroy_pd(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	struct inst_info *inst;
	int ret;
	pid_t pid;
	struct client_node *client;

	g_variant_get(param, "(i&s&s)", &pid, &pkgname, &id);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = script_handler_unload(instance_pd_handle(inst), 1);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_pinup_changed(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	int pinup;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&si)", &pid, &pkgname, &id, &pinup);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_set_pinup(inst, pinup);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_lb_mouse_down(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&siiddd)", &pid, &pkgname, &id, &w, &h, &timestamp, &x, &y);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct script_info *script;
		Evas *e;

		script = instance_lb_handle(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		evas_event_feed_mouse_in(e, timestamp, NULL);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp + 0.01f, NULL);
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
	const char *id;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	pid_t pid;
	int ret;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&siiddd)", &pid, &pkgname, &id, &w, &h, &timestamp, &x, &y);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct script_info *script;
		Evas *e;

		script = instance_lb_handle(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
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
	const char *id;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&siiddd)", &pid, &pkgname, &id, &w, &h, &timestamp, &x, &y);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct script_info *script;
		Evas *e;

		script = instance_pd_handle(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
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
	const char *id;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&siiddd)", &pid, &pkgname, &id, &w, &h, &timestamp, &x, &y);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct script_info *script;
		Evas *e;

		script = instance_pd_handle(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		evas_event_feed_mouse_up(e, 1, EVAS_BUTTON_NONE, timestamp, NULL);
		evas_event_feed_mouse_out(e, timestamp + 0.01f, NULL);
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
	const char *id;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&siiddd)", &pid, &pkgname, &id, &w, &h, &timestamp, &x, &y);
	client = client_find_by_pid(pid);
	if (!client) {
		ret = -EINVAL;
		ErrPrint("Failed to find a client\n");
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EAGAIN;
	} else {
		struct script_info *script;
		Evas *e;

		script = instance_pd_handle(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		evas_event_feed_mouse_in(e, timestamp, NULL);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp + 0.01f, NULL);
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
	const char *id;
	const char *cluster;
	const char *category;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&s&s&s)", &pid, &pkgname, &id, &cluster, &category);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EAGAIN;
	else
		ret = instance_change_group(inst, cluster, category);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_delete(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&s)", &pid, &pkgname, &id);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EAGAIN;
	} else {
		if (instance_state(inst) == INST_DEACTIVATED)
			instance_broadcast_deleted_event(inst);

		ret = instance_destroy(inst);
	}

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_resize(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	int w;
	int h;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&sii)", &pid, &pkgname, &id, &w, &h);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}


	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EAGAIN;
	else
		ret = instance_resize(inst, w, h);

out:
	param = g_variant_new("(i)", ret);
	if (!param)
		ErrPrint("Failed to create a variant\n");

	g_dbus_method_invocation_return_value(inv, param);
}

static void method_set_period(GDBusMethodInvocation *inv, GVariant *param)
{
	const char *pkgname;
	const char *id;
	double period;
	int ret;
	pid_t pid;
	struct client_node *client;
	struct inst_info *inst;

	g_variant_get(param, "(i&s&sd)", &pid, &pkgname, &id, &period);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_set_period(inst, period);

out:
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
	pid_t pid;
	double timestamp;
	struct client_node *client;
	struct pkg_info *info;

	g_variant_get(param, "(id&s&s&s&sd)", &pid, &timestamp, &pkgname, &content, &cluster, &category, &period);
	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Failed to find a client\n");
		ret = -EINVAL;
		goto out;
	}

	info = package_find(pkgname);
	if (!info)
		info = package_create(pkgname);

	if (!info) {
		ret = -EFAULT;
	} else if (package_is_fault(info)) {
		ret = -EAGAIN;
	} else {
		struct inst_info *inst;

		if (period > 0.0f && period < MINIMUM_PERIOD)
			period = MINIMUM_PERIOD;

		inst = instance_create(client, timestamp, pkgname, content, cluster, category, period);
		/*!
		 * \note
		 * Using the "inst" without validate its value is at my disposal. ;)
		 */
		ret = instance_activate(inst);
	}

out:
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
		{
			.name = "acquire",
			.method = method_acquire,
		},
		{
			.name = "release",
			.method = method_release,
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
			MASTER_OBJECT_PATH,
			s_info.node_info->interfaces[0],
			&iface_vtable,
			NULL,
			NULL,
			&err);

	if (s_info.reg_id <= 0) {
		if (err) {
			ErrPrint("register %s - %s\n", MASTER_OBJECT_PATH, err->message);
			g_error_free(err);
		}
		g_dbus_node_info_unref(s_info.node_info);
		s_info.node_info = NULL;
	}
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
}

static void on_name_lost(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
}

int dbus_init(void)
{
	int r;

	r = g_bus_own_name(BUS_TYPE,
			MASTER_SERVICE_NAME,
			G_BUS_NAME_OWNER_FLAGS_NONE,
			on_bus_acquired,
			on_name_acquired,
			on_name_lost,
			NULL, /* user_data */
			NULL /* GDestroyNotify */ );
	if (r <= 0) {
		ErrPrint("Failed to get a name: %s\n", MASTER_SERVICE_NAME);
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
