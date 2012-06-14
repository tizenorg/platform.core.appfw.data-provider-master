#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <dlog.h>
#include <Evas.h>

#include "conf.h"
#include "debug.h"
#include "server.h"
#include "packet.h"
#include "connector_packet.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "client_rpc.h"
#include "instance.h"
#include "package.h"
#include "script_handler.h"
#include "util.h"
#include "fault_manager.h"

static struct info {
	int fd;
} s_info = {
	.fd = -1,
};

static struct packet *client_acquire(pid_t pid, int handle, struct packet *packet) /*!< timestamp, ret */
{
	struct client_node *client;
	struct packet *result;
	double timestamp;
	int ret;

	client = client_find_by_pid(pid);
	if (client) {
		ErrPrint("Client is already exists %d\n", pid);
		ret = -EEXIST;
		goto out;
	}

	packet_get(packet, "d", &timestamp);
	DbgPrint("Acquired %lf\n", timestamp);

	/*!
	 * \note
	 * client_create will invoke the client created callback
	 */
	client = client_create(pid);
	if (!client) {
		ErrPrint("Failed to create a new client for %d\n", pid);
		ret = -EFAULT;
		goto out;
	}

	client_rpc_initialize(client, handle);
	ret = 0;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *cilent_release(pid_t pid, int handle, struct packet *packet) /*!< pid, ret */
{
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	client_destroy(client);
	ret = 0;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

/*!< pid, pkgname, filename, event, timestamp, x, y, ret */
static struct packet *client_clicked(pid_t pid, int handle, struct packet *packet)
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *event;
	double timestamp;
	double x;
	double y;
	int ret;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "sssddd", &pkgname, &id, &event, &timestamp, &x, &y);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

/* pid, pkgname, filename, emission, source, s, sy, ex, ey, ret */
static struct packet *client_text_signal(pid_t pid, int handle, struct packet *packet)
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *emission;
	const char *source;
	double sx;
	double sy;
	double ex;
	double ey;
	struct inst_info *inst;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssssdddd", &pkgname, &id, &emission, &source, &sx, &sy, &ex, &ey);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_delete(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct inst_info *inst;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_resize(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, w, h, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	struct inst_info *inst;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssii", &pkgname, &id, &w, &h);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_new(pid_t pid, int handle, struct packet *packet) /* pid, timestamp, pkgname, content, cluster, category, period, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *content;
	const char *cluster;
	const char *category;
	double period;
	double timestamp;
	int ret;
	struct pkg_info *info;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "dssssd", &timestamp, &pkgname, &content, &cluster, &category, &period);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_set_period(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, period, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	double period;
	int ret;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssd", &pkgname, &id, &period);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_change_group(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, cluster, category, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *cluster;
	const char *category;
	struct inst_info *inst;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &pkgname, &id, &cluster, &category);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_pd_mouse_down(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, id, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
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

		script_handler_update_pointer(script, x, y, 1);
		evas_event_feed_mouse_in(e, timestamp, NULL);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp + 0.01f, NULL);
		ret = 0;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_pd_mouse_up(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_pd_mouse_move(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_lb_mouse_move(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_lb_mouse_down(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_lb_mouse_up(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_pinup_changed(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, pinup, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int pinup;
	int ret;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pinup);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_create_pd(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_destroy_pd(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
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
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_activate_package(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	int ret;
	struct pkg_info *info;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &pkgname);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	info = package_find(pkgname);
	if (!info)
		ret = -ENOENT;
	else
		ret = package_clear_fault(info);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_livebox_is_exists(pid_t pid, int handle, struct packet *packet) /* pid, pkgname, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &pkgname);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	ret = util_validate_livebox_package(pkgname);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_hello(pid_t pid, int handle, struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &slavename);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \note
	 * After updating handle,
	 * slave activated callback will be called.
	 */
	slave_rpc_update_handle(slave, handle);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_ping(pid_t pid, int handle, struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &slavename);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	slave_rpc_ping(slave);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_call(pid_t pid, int handle, struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &func);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	ret = fault_func_call(slave, pkgname, id, func);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_ret(pid_t pid, int handle, struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &func);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	ret = fault_func_ret(slave, pkgname, id, func);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_updated(pid_t pid, int handle, struct packet *packet) /* slave_name, pkgname, filename, width, height, priority, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	double priority;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "sssiid", &slavename, &pkgname, &id, &w, &h, &priority);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

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

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_desc_updated(pid_t pid, int handle, struct packet *packet) /* slave_name, pkgname, filename, decsfile, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *descfile;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &descfile);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

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

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_deleted(pid_t pid, int handle, struct packet *packet) /* slave_name, pkgname, id, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "sss", &slavename, &pkgname, &id);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EAGAIN;
	else
		ret = instance_destroyed(inst);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct method s_table[] = {
	/*!
	 * \note
	 * service for client
	 */
	{
		.cmd = "acquire",
		.handler = client_acquire, /*!< pid, ret */
	},
	{
		.cmd = "release",
		.handler = cilent_release, /*!< pid, ret */
	},
	{
		.cmd = "clicked",
		.handler = client_clicked, /*!< pid, pkgname, filename, event, timestamp, x, y, ret */
	},
	{
		.cmd = "text_signal",
		.handler = client_text_signal, /* pid, pkgname, filename, emission, source, s, sy, ex, ey, ret */
	},
	{
		.cmd = "delete",
		.handler = client_delete, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = "resize",
		.handler = client_resize, /* pid, pkgname, filename, w, h, ret */
	},
	{
		.cmd = "new",
		.handler = client_new, /* pid, timestamp, pkgname, content, cluster, category, period, ret */
	},
	{
		.cmd = "set_period",
		.handler = client_set_period, /* pid, pkgname, filename, period, ret */
	},
	{
		.cmd = "change_group",
		.handler = client_change_group, /* pid, pkgname, filename, cluster, category, ret */
	},
	{
		.cmd = "pd_mouse_down",
		.handler = client_pd_mouse_down, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pd_mouse_up",
		.handler = client_pd_mouse_up, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pd_mouse_move",
		.handler = client_pd_mouse_move, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_move",
		.handler = client_lb_mouse_move, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_down",
		.handler = client_lb_mouse_down, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_up",
		.handler = client_lb_mouse_up, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pinup_changed",
		.handler = client_pinup_changed, /* pid, pkgname, filename, pinup, ret */
	},
	{
		.cmd = "create_pd",
		.handler = client_create_pd, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = "destroy_pd",
		.handler = client_destroy_pd, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = "activate_package",
		.handler = client_activate_package, /* pid, pkgname, ret */
	},
	{
		.cmd = "livebox_is_exists",
		.handler = client_livebox_is_exists, /* pid, pkgname, ret */
	},
	/*!
	 * \note services for slave
	 */
	{
		.cmd = "hello",
		.handler = slave_hello, /* slave_name, ret */
	},
	{
		.cmd = "ping",
		.handler = slave_ping, /* slave_name, ret */
	},
	{
		.cmd = "call",
		.handler = slave_call, /* slave_name, pkgname, filename, function, ret */
	},
	{
		.cmd = "ret",
		.handler = slave_ret, /* slave_name, pkgname, filename, function, ret */
	},
	{
		.cmd = "updated",
		.handler = slave_updated, /* slave_name, pkgname, filename, width, height, priority, ret */
	},
	{
		.cmd = "desc_updated",
		.handler = slave_desc_updated, /* slave_name, pkgname, filename, decsfile, ret */
	},
	{
		.cmd = "deleted",
		.handler = slave_deleted, /* slave_name, pkgname, filename, ret */
	},
	{
		.cmd = NULL,
		.handler = NULL,
	},
};

int server_init(void)
{
	if (unlink("/tmp/.live.socket") < 0)
		ErrPrint("unlink: %s\n", strerror(errno));

	s_info.fd = connector_packet_server_init("/tmp/.live.socket", s_table);
	if (s_info.fd < 0) {
		ErrPrint("Failed to create a server socket\n");
		return s_info.fd;
	}

	chmod("/tmp/.live.socket", 0666);
	return 0;
}

int server_fini(void)
{
	connector_packet_server_fini(s_info.fd);
	return 0;
}

/* End of a file */
