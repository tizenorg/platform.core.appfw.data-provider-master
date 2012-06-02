#include <stdio.h>
#include <string.h>
#include <stdlib.h> /* free */
#include <libgen.h> /* basename */

#include <gio/gio.h>

#include <dlog.h>
#include <Eina.h>

#include "pkg_manager.h"
#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "util.h"
#include "client_manager.h"

static struct info {
	Eina_List *call_list;
	enum {
		FAULT_NONE,
		FAULT_MARK,
		FAULT_CHECK,
	} state;
} s_info = {
	.call_list = NULL,
};

struct fault_info {
	struct slave_node *slave;
	char *pkgname;
	char *filename;
	char *func;
};

int fault_is_occured(void)
{
	return s_info.state == FAULT_CHECK;
}

int fault_check_pkgs(struct slave_node *slave)
{
	struct fault_info *info;
	Eina_List *l;
	Eina_List *n;
	int found;

	found = 0;
	EINA_LIST_FOREACH_SAFE(s_info.call_list, l, n, info) {
		if (info->slave == slave) {
			GVariant *param;
			int ret;

			ret = pkgmgr_set_fault(info->pkgname, info->filename, info->func);

			ErrPrint("Fault processing ====\n");
			ErrPrint("Slavename: %s[%d]\n", slave_name(info->slave), slave_pid(info->slave));
			ErrPrint("Package: %s\n", info->pkgname);
			ErrPrint("Filename: %s\n", info->filename);
			ErrPrint("Funcname: %s\n", info->func);
			ErrPrint("Set fault %s(%d)\n", !ret ? "Success" : "Failed", ret);

			param = g_variant_new("(sss)", info->pkgname, info->filename, info->func);
			if (param)
				client_broadcast_command("fault_package", param);
			else
				ErrPrint("Failed to create a param\n");

			s_info.call_list = eina_list_remove_list(s_info.call_list, l);

			free(info->pkgname);
			free(info->filename);
			free(info->func);
			free(info);
			found++;

			s_info.state = FAULT_CHECK;
		}
	}

	if (!found) {
		const char *pkgname;

		pkgname = pkgmgr_find_by_secure_slave(slave);
		if (!pkgname) {
			ErrPrint("Slave is crashed, but I couldn't find a fault package\n");
		} else {
			int ret;
			GVariant *param;

			ret = pkgmgr_set_fault(pkgname, NULL, NULL);
			ErrPrint("Fault processing ====\n");
			ErrPrint("Slavename: %s[%d]\n", slave_name(slave), slave_pid(slave));
			ErrPrint("Package: %s\n", pkgname);
			ErrPrint("Set fault %s(%d)\n", !ret ? "Success" : "Failed", ret);

			param = g_variant_new("(sss)", pkgname, "", "");
			if (param)
				client_broadcast_command("fault_package", param);
			else
				ErrPrint("Failed to create a param\n");

			s_info.state = FAULT_CHECK;
		}
	}

	return 0;
}

int fault_func_call(struct slave_node *slave, const char *pkgname, const char *filename, const char *func)
{
	struct fault_info *info;

	info = malloc(sizeof(*info));
	if (!info)
		return -ENOMEM;

	info->slave = slave;

	info->pkgname = strdup(pkgname);
	if (!info->pkgname) {
		free(info);
		return -ENOMEM;
	}

	info->filename = strdup(filename);
	if (!info->filename) {
		free(info->pkgname);
		free(info);
		return -ENOMEM;
	}

	info->func = strdup(func);
	if (!info->func) {
		free(info->filename);
		free(info->pkgname);
		free(info);
		return -ENOMEM;
	}

	s_info.call_list = eina_list_append(s_info.call_list, info);

	s_info.state = FAULT_MARK;
	return 0;
}

int fault_func_ret(struct slave_node *slave, const char *pkgname, const char *filename, const char *func)
{
	struct fault_info *info;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.call_list, l, info) {
		if (info->slave != slave)
			continue;

		if (strcmp(info->pkgname, pkgname))
			continue;

		if (strcmp(info->filename, filename))
			continue;

		if (strcmp(info->func, func))
			continue;

		s_info.call_list = eina_list_remove_list(s_info.call_list, l);
		free(info->filename);
		free(info->pkgname);
		free(info->func);
		free(info);

		s_info.state = FAULT_NONE;
		return 0;
	} 

	return -ENOENT;
}

/* End of a file */
