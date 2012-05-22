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

static void create_return_cb(const char *funcname, GVariant *param, int ret, void *data)
{
	struct inst_info *inst;
	const char *pkgname;
	const char *filename;
	const char *cluster;
	const char *category;
	const char *content;

	g_variant_unref(param);

	if (ret < 0) {
		/*!
		 * \WARN: Error occured
		 * And the "inst" is already DELETED
		 */
	} else if (ret == 0) { /* DONE */
		/* OKAY, There is no changes */
	} else if (ret & 0x01) { /* NEED_TO_CREATE */
		/* System send create request */
		inst = data;

		pkgname = pkgmgr_name(inst);
		filename = pkgmgr_filename(inst);
		cluster = pkgmgr_cluster(inst);
		category = pkgmgr_category(inst);
		content = pkgmgr_content(inst);

		inst = rpc_send_create_request(NULL, pkgname, content, cluster, category, util_get_timestamp());
		if (inst) {
			/* Send create livebox again */
			DbgPrint("Send create request again, it requires\n");
			DbgPrint("pkgname: %s, cluster: %s, category: %s\n", pkgname, cluster, category);
		}
	}
}

int rpc_send_new(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *param, int ret, void *data), void *data, int skip_need_to_create)
{
	struct slave_node *slave;
	GVariant *param;
	int ret;

	param = g_variant_new("(sssiidssii)",
			pkgmgr_name(inst),
			pkgmgr_filename(inst),
			pkgmgr_content(inst),
			pkgmgr_timeout(inst),
			!!pkgmgr_lb_path(inst),
			pkgmgr_period(inst),
			pkgmgr_cluster(inst),
			pkgmgr_category(inst),
			pkgmgr_pinup(inst),
			skip_need_to_create);
	if (!param)
		return -EFAULT;

	slave = pkgmgr_slave(pkgmgr_name(inst));
	if (!slave) {
		ErrPrint("Slave is not found\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	ret = slave_push_command(slave, pkgmgr_name(inst), pkgmgr_filename(inst), "new", param, ret_cb, data);
	if (ret < 0)
		g_variant_unref(param);

	return ret;
}

int rpc_send_renew(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *param, int ret, void *data), void *data)
{
	struct slave_node *slave;
	GVariant *param;
	int ret;
	int w;
	int h;

	pkgmgr_get_size(inst, &w, &h, 0);
	param = g_variant_new("(sssiidssiii)",
			pkgmgr_name(inst),
			pkgmgr_filename(inst),
			pkgmgr_content(inst),
			pkgmgr_timeout(inst),
			!!pkgmgr_lb_path(inst),
			pkgmgr_period(inst),
			pkgmgr_cluster(inst),
			pkgmgr_category(inst),
			pkgmgr_pinup(inst),
			w, h);
	if (!param)
		return -EFAULT;

	slave = pkgmgr_slave(pkgmgr_name(inst));
	if (!slave) {
		ErrPrint("Slave is not found\n");
		g_variant_unref(param);
		return -EFAULT;
	}

	ret = slave_push_command(slave, pkgmgr_name(inst), pkgmgr_filename(inst), "renew", param, ret_cb, data);
	if (ret < 0)
		g_variant_unref(param);

	return ret;
}

struct inst_info *rpc_send_create_request(struct client_node *client, const char *pkgname, const char *content, const char *cluster, const char *category, double timestamp)
{
	char *filename;
	int fnlen;
	struct inst_info *inst;
	struct slave_node *slave;
	int ret;

	fnlen = 256 + strlen(g_conf.path.image);
	filename = malloc(fnlen);
	if (!filename) {
		ErrPrint("Failed to allocate memory for filename: %d\n", fnlen);
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
	int ret;

	slave = pkgmgr_slave(pkgname);
	if (!slave)
		return;

	param = g_variant_new("(sss)", pkgname, cluster, category);
	if (!param) {
		ErrPrint("Failed to create a new param\n");
		return;
	}

	ret = slave_push_command(slave, pkgname, NULL, "update_content", param, NULL, NULL);
	if (ret < 0)
		g_variant_unref(param);
}

void rpc_send_pause_request(void)
{
	GVariant *param;

	param = g_variant_new("(d)", util_get_timestamp());
	if (!param)
		return;

	slave_broadcast_command("pause", param);
}

void rpc_send_resume_request(void)
{
	GVariant *param;

	param = g_variant_new("(d)", util_get_timestamp());
	if (!param)
		return;

	slave_broadcast_command("resume", param);
}

/* End of a file */
