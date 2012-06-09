#include <stdio.h>
#include <string.h>
#include <stdlib.h> /* free */
#include <libgen.h> /* basename */

#include <gio/gio.h>

#include <dlog.h>
#include <Eina.h>

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "util.h"
#include "client_life.h"
#include "client_rpc.h"
#include "package.h"

static struct info {
	Eina_List *call_list;
	int fault_mark_count;
} s_info = {
	.call_list = NULL,
	.fault_mark_count = 0,
};

struct fault_info {
	struct slave_node *slave;
	double timestamp;
	char *pkgname;
	char *filename;
	char *func;
};

int const fault_is_occured(void)
{
	return s_info.fault_mark_count;
}

static void clear_log_file(struct slave_node *slave)
{
	char filename[BUFSIZ];
	snprintf(filename, sizeof(filename), "/opt/share/live_magazine/log/slave.%d", slave_pid(slave));

	unlink(filename);
}

static char *check_log_file(struct slave_node *slave)
{
	char pkgname[BUFSIZ];
	const char *pattern = "liblive-";
	char *ptr;
	FILE *fp;
	int i;
	char filename[BUFSIZ];

	snprintf(filename, sizeof(filename), "/opt/share/live_magazine/log/slave.%d", slave_pid(slave));
	fp = fopen(filename, "rt");
	if (!fp) {
		ErrPrint("No log file found [%s]\n", strerror(errno));
		return NULL;
	}

	ptr = fgets(pkgname, sizeof(pkgname), fp);
	fclose(fp);
	if (ptr != pkgname) {
		ErrPrint("Invalid log\n");
		return NULL;
	}

	for (i = 0; pattern[i] && (pattern[i] == pkgname[i]); i++); /*!< Check pattern of filename */
	if (strlen(pattern) != i) {
		ErrPrint("Pattern is not matched: %d\n", i);
		return NULL;
	}

	ptr = pkgname + i;
	i = strlen(ptr) - 3; /* Skip the ".so" */
	if (i <= 0 || strcmp(ptr + i, ".so")) {
		ErrPrint("Extension is not matched\n");
		return NULL;
	}
		
	ptr[i] = '\0'; /*!< Truncate tailer ".so" */
	if (unlink(filename) < 0)
		ErrPrint("Failed to unlink %s\n", filename);

	return strdup(ptr);
}

void fault_unicast_info(struct client_node *client, const char *pkgname, const char *filename, const char *func)
{
	GVariant *param;

	param = g_variant_new("(sss)", pkgname, filename, func);
	if (!param)
		return;

	client_rpc_async_request(client, "fault_package", param);
	DbgPrint("Fault package: %s\n", pkgname);
}

void fault_broadcast_info(const char *pkgname, const char *filename, const char *func)
{
	GVariant *param;

	param = g_variant_new("(sss)", pkgname, filename, func);
	if (!param) {
		ErrPrint("Failed to create a param\n");
		return;
	}

	client_rpc_broadcast("fault_package", param);
	DbgPrint("Fault package: %s\n", pkgname);
}

static inline void dump_fault_info(const char *name, pid_t pid, const char *pkgname, const char *filename, const char *funcname)
{
	ErrPrint("Fault processing ====\n");
	ErrPrint("Slavename: %s[%d]\n", name, pid);
	ErrPrint("Package: %s\n", pkgname);
	ErrPrint("Filename: %s\n", filename);
	ErrPrint("Funcname: %s\n", funcname);
}

int fault_check_pkgs(struct slave_node *slave)
{
	struct fault_info *info;
	struct pkg_info *pkg;
	const char *pkgname;
	Eina_List *l;
	Eina_List *n;
	int checked;

	/*!
	 * \note
	 * First step.
	 * Check the log file
	 */
	pkgname = (const char *)check_log_file(slave);
	if (pkgname) {
		pkg = package_find(pkgname);
		if (pkg) {
			int ret;
			ret = package_set_fault_info(pkg, util_timestamp(), NULL, NULL);
			dump_fault_info(slave_name(slave), slave_pid(slave), pkgname, "", "");
			ErrPrint("Set fault %s(%d)\n", !ret ? "Success" : "Failed", ret);
			fault_broadcast_info(pkgname, "", "");
			s_info.fault_mark_count = 0;
			return 0;
		}
	}

	/*!
	 * \note
	 * Second step.
	 * Is it secured slave?
	 */
	pkgname = package_find_by_secured_slave(slave);
	if (pkgname) {
		pkg = package_find(pkgname);
		if (pkg) {
			int ret;
			ret = package_set_fault_info(pkg, util_timestamp(), NULL, NULL);
			dump_fault_info(slave_name(slave), slave_pid(slave), pkgname, "", "");
			ErrPrint("Set fault %s(%d)\n", !ret ? "Success" : "Failed", ret);
			fault_broadcast_info(pkgname, "", "");
			s_info.fault_mark_count = 0;
			clear_log_file(slave);
			return 0;
		}
	}

	/*!
	 * \note
	 * At last, check the pair of function call and return mark
	 */
	checked = 0;
	EINA_LIST_REVERSE_FOREACH_SAFE(s_info.call_list, l, n, info) {
		if (info->slave == slave) {
			const char *filename;
			const char *func;

			pkg = package_find(info->pkgname);
			if (!pkg) {
				ErrPrint("Failed to find a package %s\n", info->pkgname);
				continue;
			}

			filename = info->filename ? info->filename : "";
			func = info->func ? info->func : "";

			if (!checked) {
				int ret;
				ret = package_set_fault_info(pkg, info->timestamp, info->filename, info->func);
				fault_broadcast_info(info->pkgname, info->filename, info->func);
				ErrPrint("Set fault %s(%d)\n", !ret ? "Success" : "Failed", ret);
			} else {
				DbgPrint("Treated as a false log\n");
				dump_fault_info(
					slave_name(info->slave), slave_pid(info->slave), info->pkgname, filename, func);
			}

			s_info.call_list = eina_list_remove_list(s_info.call_list, l);

			free(info->pkgname);
			free(info->filename);
			free(info->func);
			free(info);
			checked = 1;
		}
	}

	s_info.fault_mark_count = 0;
	clear_log_file(slave);
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

	info->timestamp = util_timestamp();

	s_info.call_list = eina_list_append(s_info.call_list, info);

	s_info.fault_mark_count++;
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

		s_info.fault_mark_count--;
		return 0;
	} 

	return -ENOENT;
}

/* End of a file */
