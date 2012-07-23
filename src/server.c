#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <dlog.h>
#include <Evas.h>
#include <Ecore_Evas.h> /* fb.h */

#include <packet.h>
#include <com-core_packet.h>

#include "conf.h"
#include "debug.h"
#include "server.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "script_handler.h"
#include "buffer_handler.h"
#include "util.h"
#include "fault_manager.h"
#include "fb.h" /* fb_type */
#include "group.h"

static struct info {
	int fd;
} s_info = {
	.fd = -1,
};

/* Share this with provider */
enum target_type {
	TYPE_LB,
	TYPE_PD,
	TYPE_ERROR,
};

enum buffer_method {
	BUFFER_SHM,
	BUFFER_FILE,
	BUFFER_ERROR,
};

static struct packet *client_acquire(pid_t pid, int handle, const struct packet *packet) /*!< timestamp, ret */
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

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid arguemnt\n");
		ret = -EINVAL;
		goto out;
	}

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

static struct packet *cilent_release(pid_t pid, int handle, const struct packet *packet) /*!< pid, ret */
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
static struct packet *client_clicked(pid_t pid, int handle, const struct packet *packet)
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
static struct packet *client_text_signal(pid_t pid, int handle, const struct packet *packet)
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
		ret = -EFAULT;
	else
		ret = instance_text_signal_emit(inst, emission, source, sx, sy, ex, ey);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_delete(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
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
		ret = -EFAULT;
	} else {
		ret = instance_destroy(inst);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_resize(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, w, h, ret */
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
		ret = -EFAULT;
	else
		ret = instance_resize(inst, w, h);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_new(pid_t pid, int handle, const struct packet *packet) /* pid, timestamp, pkgname, content, cluster, category, period, ret */
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
		ret = -EFAULT;
	} else {
		struct inst_info *inst;

		if (period > 0.0f && period < MINIMUM_PERIOD)
			period = MINIMUM_PERIOD;

		if (!strlen(content))
			content = DEFAULT_CONTENT;
		inst = instance_create(client, timestamp, pkgname, content, cluster, category, period);
		/*!
		 * \note
		 * Using the "inst" without validate its value is at my disposal. ;)
		 */
		ret = inst ? 0 : -EFAULT;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_set_period(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, period, ret */
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
		period = -1.0f;
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
	result = packet_create_reply(packet, "id", ret, period);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_change_group(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, cluster, category, ret */
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
		ret = -EFAULT;
	else
		ret = instance_change_group(inst, cluster, category);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_pd_mouse_down(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, id, width, height, timestamp, x, y, ret */
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
	const struct pkg_info *pkg;

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
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		packet = packet_create("pd_mouse_down", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}

		ret = slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
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

static struct packet *client_pd_mouse_up(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
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
	const struct pkg_info *pkg;

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
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		packet = packet_create("pd_mouse_up", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}

		ret = slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
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

static struct packet *client_pd_mouse_move(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
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
	const struct pkg_info *pkg;

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
		ErrPrint("Instance[%s] is not exists\n");
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n");
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		packet = packet_create("pd_mouse_move", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}

		ret = slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
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

static struct packet *client_lb_mouse_move(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
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
	const struct pkg_info *pkg;

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
		ErrPrint("Instance[%s] is not exists\n");
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		packet = packet_create("lb_mouse_move", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}

		ret = slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
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

static struct packet *client_lb_mouse_down(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
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
	const struct pkg_info *pkg;

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
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		packet = packet_create("lb_mouse_down", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}

		ret = slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
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

static struct packet *client_lb_mouse_up(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
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
	const struct pkg_info *pkg;

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
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		packet = packet_create("lb_mouse_up", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}

		ret = slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
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

static struct packet *client_pinup_changed(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, pinup, ret */
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
		pinup = 0;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pinup);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		pinup = 0;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else {
		ret = instance_set_pinup(inst, pinup);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static inline int slave_send_pd_create(struct inst_info *inst)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	const struct pkg_info *info;

	slave = package_slave(instance_package(inst));
	if (!slave)
		return -EFAULT;

	info = instance_package(inst);
	if (!info)
		return -EINVAL;

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id)
		return -EINVAL;

	packet = packet_create("pd_show", "ssii", pkgname, id, instance_pd_width(inst), instance_pd_height(inst));
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
}

static inline int slave_send_pd_destroy(struct inst_info *inst)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	struct pkg_info *info;

	slave = package_slave(instance_package(inst));
	if (!slave)
		return -EFAULT;

	info = instance_package(inst);
	if (!info)
		return -EINVAL;

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id)
		return -EINVAL;

	packet = packet_create("pd_hide", "ss", pkgname, id);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return slave_rpc_async_request(slave, pkgname, packet, NULL, NULL);
}

static struct packet *client_create_pd(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
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
	else if (package_pd_type(instance_package(inst)) == PD_TYPE_BUFFER)
		ret = slave_send_pd_create(inst);
	else
		ret = script_handler_load(instance_pd_script(inst), 1);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_destroy_pd(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
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
	else if (package_pd_type(instance_package(inst)) == PD_TYPE_BUFFER)
		ret = slave_send_pd_destroy(inst);
	else
		ret = script_handler_unload(instance_pd_script(inst), 1);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_activate_package(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, ret */
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
		pkgname = "";
		goto out;
	}

	ret = packet_get(packet, "s", &pkgname);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		pkgname = "";
		goto out;
	}

	info = package_find(pkgname);
	if (!info)
		ret = -ENOENT;
	else
		ret = package_clear_fault(info);

out:
	result = packet_create_reply(packet, "is", ret, pkgname);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_subscribed(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \todo
	 * SUBSCRIBE cluster & sub-cluster for a client.
	 */
	ret = client_subscribe(client, cluster, category);

	if (ret == 0)
		package_alter_instances_to_client(client);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_delete_cluster(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &cluster);
	if (ret != 1) {
		ErrPrint("Invalid parameters\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = -ENOSYS;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");
	return result;
}

static int update_pkg_cb(struct category *category, const char *pkgname, void *data)
{
	const char *c_name;
	const char *s_name;

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid\n");
		return EXIT_FAILURE;
	}

	slave_rpc_request_update(pkgname, c_name, s_name);
	return EXIT_SUCCESS;
}

static struct packet *client_refresh_group(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster_id;
	const char *category_id;
	struct client_node *client;
	struct packet *result;
	int ret;
	struct cluster *cluster;
	struct category *category;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		ret = -ENOMEM;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster_id, &category_id);
	if (ret != 2) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	cluster = group_find_cluster(cluster_id);
	if (!cluster) {
		ErrPrint("Cluster [%s] is not registered\n", cluster_id);
		ret = -EINVAL;
		goto out;
	}

	category = group_find_category(cluster, category_id);
	if (!category) {
		ErrPrint("Category [%s] is not registered\n", category_id);
		ret = -EINVAL;
		goto out;
	}

	group_list_category_pkgs(category, update_pkg_cb, NULL);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");
	return result;
}

static struct packet *client_delete_category(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid paramenters\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = -ENOSYS;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");
	return result;
}

static struct packet *client_unsubscribed(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid paramenters\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \todo
	 * UNSUBSCRIBE cluster & sub-cluster for a client.
	 */
	ret = client_unsubscribe(client, cluster, category);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");
	return result;
}

static struct packet *client_livebox_is_exists(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, ret */
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

static struct packet *slave_hello(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave[%d] is not exists\n", pid);
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

static struct packet *slave_ping(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	struct packet *result;
	const char *slavename;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
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

static struct packet *slave_call(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
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
		ErrPrint("Slave %d is not exists\n", pid);
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

static struct packet *slave_ret(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
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
		ErrPrint("Slave %d is not exists\n", pid);
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

static struct packet *slave_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, width, height, priority, ret */
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
		ErrPrint("Slave %d is not exists\n", pid);
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
		ret = -EFAULT;
	} else if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Instance is already destroyed\n");
		ret = -EINVAL;
	} else {
		instance_set_lb_info(inst, w, h, priority);

		if (package_lb_type(instance_package(inst)) == LB_TYPE_SCRIPT) {
			script_handler_resize(instance_lb_script(inst), w, h);
			ret = script_handler_parse_desc(pkgname, id, URI_TO_PATH(id), 0);
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

static struct packet *slave_desc_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, decsfile, ret */
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
		ErrPrint("Slave %d is not exists\n", pid);
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
		ret = -EFAULT;
	} else if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Instance is already destroyed\n");
		ret = -EINVAL;
	} else if (package_pd_type(instance_package(inst)) == PD_TYPE_TEXT) {
		instance_set_pd_info(inst, 0, 0);
		instance_pd_updated(pkgname, id, descfile);
		ret = 0;
	} else if (script_handler_is_loaded(instance_pd_script(inst))) {
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

static struct packet *slave_deleted(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, id, ret */
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
		ErrPrint("Slave %d is not exists\n", pid);
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
		ret = -EFAULT;
	else
		ret = instance_destroyed(inst);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

/*!
 * \note for the BUFFER Type slave
 */
static struct packet *slave_acquire_buffer(pid_t pid, int handle, const struct packet *packet) /* type, id, w, h, size */
{
	enum target_type target;
	const char *slavename;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	int pixel_size;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		return NULL;
	}

	if (packet_get(packet, "isssiii", &target, &slavename, &pkgname, &id, &w, &h, &pixel_size) != 7) {
		ErrPrint("Invalid argument\n");
		return NULL;
	}

	/* TODO: */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		result = packet_create_reply(packet, "is", BUFFER_ERROR, "");
		if (!result)
			ErrPrint("Failed to create a packet\n");

		return result;
	}

	pkg = instance_package(inst);
	id = "";
	ret = -EINVAL;
	if (target == TYPE_LB) {
		if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
			struct buffer_info *info;

			info = instance_lb_buffer(inst);
			if (!info)
				instance_create_lb_buffer(inst);

			ret = buffer_handler_resize(info, w, h);

			if (buffer_handler_load(info) == 0)
				id = buffer_handler_id(info);
		}
	} else if (target == TYPE_PD) {
		if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
			struct buffer_info *info;

			info = instance_pd_buffer(inst);
			if (!info)
				instance_create_pd_buffer(inst);

			ret = buffer_handler_resize(info, w, h);

			if (buffer_handler_load(info) == 0)
				id = buffer_handler_id(info);
		}
	}

	result = packet_create_reply(packet, "is", ret, id);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_resize_buffer(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct packet *result;
	enum target_type type;
	const char *slavename;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		return NULL;
	}

	if (packet_get(packet, "isssii", &type, &slavename, &pkgname, &id, &w, &h) != 6) {
		ErrPrint("Invalid argument\n");
		result = packet_create_reply(packet, "i", -EINVAL);
		if (!result)
			ErrPrint("Failed to create a packet\n");

		return result;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		DbgPrint("Instance is not found[%s] [%s]\n", pkgname, id);
		result = packet_create_reply(packet, "i", -ENOENT);
		if (!result)
			ErrPrint("Failed to create a packet\n");

		return result;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		/*!
		 * \note
		 * THIS statement should not be entered.
		 */
		ErrPrint("PACKAGE INFORMATION IS NOT VALID\n");
		result = packet_create_reply(packet, "i", -EFAULT);
		if (!result)
			ErrPrint("Failed to create a packet\n");
		return result;
	}

	ret = -EINVAL;
	id = "";
	if (type == TYPE_LB) {
		if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
			struct buffer_info *info;
			info = instance_lb_buffer(inst);
			ret = buffer_handler_resize(info, w, h);
			/*!
			 * \note
			 * id is resued for newly assigned ID
			 */
			if (!ret)
				id = buffer_handler_id(info);
		}
	} else if (type == TYPE_PD) {
		if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
			struct buffer_info *info;
			info = instance_pd_buffer(inst);
			ret = buffer_handler_resize(info, w, h);
			/*!
			 * \note
			 * id is resued for newly assigned ID
			 */
			if (!ret)
				id = buffer_handler_id(info);
		}
	}

	result = packet_create_reply(packet, "is", ret, id);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_release_buffer(pid_t pid, int handle, const struct packet *packet)
{
	enum target_type type;
	const char *slavename;
	const char *pkgname;
	const char *id;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		return NULL;
	}

	if (packet_get(packet, "isss", &type, &slavename, &pkgname, &id) != 4) {
		ErrPrint("Inavlid argument\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance is not found [%s - %s]\n", pkgname, id);
		ret = -ENOENT;
		goto out;
	}

	ret = -EINVAL;
	if (type == TYPE_LB) {
		struct buffer_info *info;

		info = instance_lb_buffer(inst);
		ret = buffer_handler_unload(info);
	} else if (type == TYPE_PD) {
		struct buffer_info *info;

		info = instance_lb_buffer(inst);
		ret = buffer_handler_unload(info);
	}

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
		.handler = client_set_period, /* pid, pkgname, filename, period, ret, period */
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
	{
		.cmd = "subscribe", /* pid, cluster, sub-cluster */
		.handler = client_subscribed,
	},
	{
		.cmd = "unsubscribe", /* pid, cluster, sub-cluster */
		.handler = client_unsubscribed,
	},
	{
		.cmd = "delete_cluster",
		.handler = client_delete_cluster,
	},
	{
		.cmd = "delete_category",
		.handler = client_delete_category,
	},
	{
		.cmd = "refresh_group",
		.handler = client_refresh_group,
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
		.cmd = "acquire_buffer",
		.handler = slave_acquire_buffer, /* slave_name, id, w, h, size, - out - type, shmid */
	},
	{
		.cmd = "resize_buffer",
		.handler = slave_resize_buffer,
	},
	{
		.cmd = "release_buffer",
		.handler = slave_release_buffer, /* slave_name, id - ret */
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

	s_info.fd = com_core_packet_server_init("/tmp/.live.socket", s_table);
	if (s_info.fd < 0) {
		ErrPrint("Failed to create a server socket\n");
		return s_info.fd;
	}

	chmod("/tmp/.live.socket", 0666);
	return 0;
}

int server_fini(void)
{
	com_core_packet_server_fini(s_info.fd);
	return 0;
}

/* End of a file */
