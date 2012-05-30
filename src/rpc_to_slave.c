#include <stdio.h>
#include <errno.h>
#include <libgen.h>
#include <string.h>
#include <stdlib.h>

#include <gio/gio.h>

#include <dlog.h>

#include "pkg_manager.h"
#include "util.h"
#include "debug.h"
#include "rpc_to_slave.h"
#include "slave_manager.h"
#include "conf.h"
#include "client_manager.h"

int errno;

static void create_return_cb(const char *funcname, GVariant *result, void *data)
{
	struct inst_info *inst;
	double priority;
	int w, h;
	int ret;
	const char *pkgname;
	const char *filename;

	inst = data;
	g_variant_get(result, "(iiid)", &ret, &w, &h, &priority);
	g_variant_unref(result);

	pkgname = pkgmgr_name(inst);
	filename = pkgmgr_filename(inst);

	/*!
	 * \note
	 * ret == 0 : need_to_create 0
	 * ret == 1 : need_to_create 1
	 */
	if (ret == 0) { /* DONE */
		pkgmgr_set_info(inst, w, h, priority);
		pkgmgr_created(pkgname, filename);
	} else if (ret == 1) { /* NEED_TO_CREATE */
		const char *cluster;
		const char *category;
		const char *content;
		double period;

		pkgmgr_set_info(inst, w, h, priority);
		pkgmgr_created(pkgname, filename);

		/* Send create request again */
		cluster = pkgmgr_cluster(inst);
		category = pkgmgr_category(inst);
		content = pkgmgr_content(inst);
		period = pkgmgr_period(inst);

		/* Send create livebox again */
		inst = rpc_send_create_request(NULL, pkgname, content, cluster, category, util_get_timestamp(), period);
		if (!inst)
			ErrPrint("Failed to send a create request for %s(%s, %s)\n", pkgname, cluster, category);
	} else if (ret < 0) {
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
			param = g_variant_new("(ss)", pkgname, filename);
			if (param)
				client_push_command(client, "deleted", param);
		}

		pkgmgr_delete(inst);
	}
}

/*!
 * \note
 * Send "new" livebox reqeust to slave data provider,
 * after send the request, "ret_cb" will be invoked.
 */
int rpc_send_new(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *result, void *data), void *data, int skip_need_to_create)
{
	struct slave_node *slave;
	GVariant *param;
	int ret;
	const char *pkgname;
	const char *filename;

	pkgname = pkgmgr_name(inst);
	filename = pkgmgr_filename(inst);

	param = g_variant_new("(sssiidssiis)",
			pkgname,
			filename,
			pkgmgr_content(inst),
			pkgmgr_timeout(inst),
			!!pkgmgr_lb_path(inst),
			pkgmgr_period(inst),
			pkgmgr_cluster(inst),
			pkgmgr_category(inst),
			pkgmgr_pinup(inst),
			skip_need_to_create,
			pkgmgr_abi(inst));
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	slave = pkgmgr_slave(pkgname);
	if (!slave) {
		ErrPrint("Slave is not found\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	ret = slave_push_command(slave, pkgname, filename, "new", param, ret_cb, data);
	return ret;
}

int rpc_send_renew(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *result, void *data), void *data)
{
	struct slave_node *slave;
	GVariant *param;
	int ret;
	int w;
	int h;
	const char *pkgname;
	const char *filename;

	pkgname = pkgmgr_name(inst);
	filename = pkgmgr_filename(inst);

	pkgmgr_get_size(inst, &w, &h, 0);
	param = g_variant_new("(sssiidssiiis)",
			pkgname,
			filename,
			pkgmgr_content(inst),
			pkgmgr_timeout(inst),
			!!pkgmgr_lb_path(inst),
			pkgmgr_period(inst),
			pkgmgr_cluster(inst),
			pkgmgr_category(inst),
			pkgmgr_pinup(inst),
			w, h,
			pkgmgr_abi(inst));
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return -EFAULT;
	}

	slave = pkgmgr_slave(pkgname);
	if (!slave) {
		ErrPrint("Slave is not found\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	ret = slave_push_command(slave, pkgname, filename, "renew", param, ret_cb, data);
	return ret;
}

struct inst_info *rpc_send_create_request(struct client_node *client, const char *pkgname, const char *content, const char *cluster, const char *category, double timestamp, double period)
{
	char *filename;
	int fnlen;
	struct inst_info *inst;
	struct slave_node *slave;
	int ret;

	fnlen = 256 + strlen(g_conf.path.image);
	filename = malloc(fnlen);
	if (!filename) {
		ErrPrint("Heap: %s (%d)\n", strerror(errno), fnlen);
		return NULL;
	}

	snprintf(filename, fnlen, "%s%d_%lf.png", g_conf.path.image, getpid(), timestamp);

	inst = pkgmgr_new(timestamp, pkgname, filename, content, cluster, category);
	free(filename);
	if (!inst) {
		ErrPrint("Failed to create a new instance\n");
		return NULL;
	}

	ret = pkgmgr_set_client(inst, client);
	if (ret < 0) {
		ErrPrint("Failed to set client\n");
		pkgmgr_delete(inst);
		return NULL;
	}

	if (period >= 0.0f) {
		if (period > 0.0f && period < MINIMUM_PERIOD)
			period = MINIMUM_PERIOD;

		pkgmgr_set_period(inst, period);
	} /* else use the default period */

	/* This package has no client */
	slave = pkgmgr_slave(pkgname);
	if (!slave) {
		if (!pkgmgr_is_secured(pkgname))
			slave = slave_find_usable();

		if (!slave) {
			char tmpname[BUFSIZ];
			snprintf(tmpname, sizeof(tmpname), "%lf", util_get_timestamp());
			slave = slave_create(tmpname, pkgmgr_is_secured(pkgname));
		}

		if (!slave) {
			ErrPrint("Failed to get proper slave object\n");
			pkgmgr_delete(inst);
			return NULL;
		}

		ret = pkgmgr_set_slave(pkgname, slave);
		if (ret < 0) {
			ErrPrint("Failed to set slave\n");
			pkgmgr_delete(inst);
			return NULL;
		}
	}

	ret = rpc_send_new(inst, create_return_cb, inst, !!client);
	if (ret < 0) {
		ErrPrint("Failed to send create request\n");
		pkgmgr_delete(inst);
		return NULL;
	}

	return inst;
}

void rpc_send_update_request(const char *pkgname, const char *cluster, const char *category)
{
	struct slave_node *slave;
	GVariant *param;

	slave = pkgmgr_slave(pkgname);
	if (!slave)
		return;

	param = g_variant_new("(sss)", pkgname, cluster, category);
	if (!param) {
		ErrPrint("Failed to create a new param\n");
		return;
	}

	(void)slave_push_command(slave, pkgname, NULL, "update_content", param, NULL, NULL);
}

void rpc_send_pause_request(void)
{
	GVariant *param;

	param = g_variant_new("(d)", util_get_timestamp());
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return;
	}

	slave_broadcast_command("pause", param);
}

void rpc_send_resume_request(void)
{
	GVariant *param;

	param = g_variant_new("(d)", util_get_timestamp());
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return;
	}

	slave_broadcast_command("resume", param);
}

/* End of a file */
