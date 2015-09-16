/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>

#include <dlog.h>
#include <Eina.h>
#include <sqlite3.h>
#include <db-util.h>
#include <widget_errno.h>
#include <widget_service.h>
#include <widget_service_internal.h>
#include <widget_conf.h>

#include "debug.h"
#include "conf.h"
#include "parser.h"
#include "group.h"
#include "util.h"
#include "client_life.h"
#include "slave_life.h"
#include "package.h"
#include "io.h"

int errno;

static struct {
	sqlite3 *handle;
} s_info = {
	.handle = NULL,
};

static inline int build_client_info(struct pkg_info *info)
{
	static const char *dml = "SELECT auto_launch, gbar_size FROM client WHERE pkgid = ?";
	sqlite3_stmt *stmt;
	int width;
	int height;
	int ret;
	const char *tmp;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname %s\n", package_name(info));
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no records (%s)\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	package_set_auto_launch(info, (const char *)sqlite3_column_text(stmt, 0));

	tmp = (const char *)sqlite3_column_text(stmt, 1);
	if (tmp && strlen(tmp)) {
		if (sscanf(tmp, "%dx%d", &width, &height) != 2) {
			ErrPrint("Failed to get GBAR width and Height (%s)\n", tmp);
		} else {
			package_set_gbar_width(info, width);
			package_set_gbar_height(info, height);
		}
	}

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return WIDGET_ERROR_NONE;
}

static inline int build_provider_info(struct pkg_info *info)
{
	static const char *dml = "SELECT provider.network, provider.abi, provider.secured, provider.box_type, provider.box_src, provider.box_group, provider.gbar_type, provider.gbar_src, provider.gbar_group, provider.libexec, provider.timeout, provider.period, provider.script, provider.pinup, pkgmap.appid, provider.direct_input, provider.hw_acceleration, pkgmap.category, provider.auto_align FROM provider, pkgmap WHERE pkgmap.pkgid = ? AND provider.pkgid = ?";
	sqlite3_stmt *stmt;
	int ret;
	const char *tmp;
	const char *appid;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	if (sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname(%s) - %s\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	if (sqlite3_bind_text(stmt, 2, package_name(info), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname(%s) - %s\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no record(%s)\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	appid = (const char *)sqlite3_column_text(stmt, 14);
	if (!appid || !strlen(appid)) {
		ErrPrint("Failed to execute the DML for %s\n", package_name(info));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	package_set_network(info, sqlite3_column_int(stmt, 0));
	package_set_secured(info, sqlite3_column_int(stmt, 2));

	tmp = (const char *)sqlite3_column_text(stmt, 1);
	if (tmp && strlen(tmp)) {
		package_set_abi(info, tmp);
	}

	package_set_widget_type(info, sqlite3_column_int(stmt, 3));
	tmp = (const char *)sqlite3_column_text(stmt, 4);
	if (tmp && strlen(tmp)) {
		package_set_widget_path(info, tmp);

		tmp = (const char *)sqlite3_column_text(stmt, 5);
		if (tmp && strlen(tmp)) {
			package_set_widget_group(info, tmp);
		}
	}

	package_set_gbar_type(info, sqlite3_column_int(stmt, 6));
	tmp = (const char *)sqlite3_column_text(stmt, 7);
	if (tmp && strlen(tmp)) {
		package_set_gbar_path(info, tmp);

		tmp = (const char *)sqlite3_column_text(stmt, 8);
		if (tmp && strlen(tmp)) {
			package_set_gbar_group(info, tmp);
		}
	}

	tmp = (const char *)sqlite3_column_text(stmt, 9);
	if (tmp && strlen(tmp)) {
		package_set_libexec(info, tmp);
	}

	package_set_timeout(info, sqlite3_column_int(stmt, 10));

	tmp = (const char *)sqlite3_column_text(stmt, 11);
	if (tmp && strlen(tmp)) {
		package_set_period(info, atof(tmp));
	}

	tmp = (const char *)sqlite3_column_text(stmt, 12);
	if (tmp && strlen(tmp)) {
		package_set_script(info, tmp);
	}

	package_set_pinup(info, sqlite3_column_int(stmt, 13));
	package_set_direct_input(info, sqlite3_column_int(stmt, 15));
	package_set_hw_acceleration(info, (const char *)sqlite3_column_text(stmt, 16));
	package_set_category(info, (const char *)sqlite3_column_text(stmt, 17));
	package_set_auto_align(info, sqlite3_column_int(stmt, 18));

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return WIDGET_ERROR_NONE;
}

static inline int build_box_size_info(struct pkg_info *info)
{
	static const char *dml = "SELECT size_type FROM box_size WHERE pkgid = ?";
	sqlite3_stmt *stmt;
	int ret;
	unsigned int size_type;
	unsigned int size_list;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	if (sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname(%s) - %s\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	size_list = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		size_type = sqlite3_column_int(stmt, 0);
		size_list |= size_type;
	}

	package_set_size_list(info, size_list);

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return WIDGET_ERROR_NONE;
}

static inline int load_context_option(struct context_item *item, int id)
{
	static const char *dml = "SELECT key, value FROM option WHERE option_id = ?";
	sqlite3_stmt *stmt;
	const char *key;
	const char *value;
	int ret;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = sqlite3_bind_int(stmt, 1, id);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = WIDGET_ERROR_IO_ERROR;
		goto out;
	}

	ret = WIDGET_ERROR_NOT_EXIST;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		key = (const char *)sqlite3_column_text(stmt, 0);
		if (!key || !strlen(key)) {
			ErrPrint("KEY is nil\n");
			continue;
		}

		value = (const char *)sqlite3_column_text(stmt, 1);
		if (!value || !strlen(value)) {
			ErrPrint("VALUE is nil\n");
			continue;
		}

		ret = group_add_option(item, key, value);
		if (ret < 0) {
			break;
		}
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int load_context_item(struct context_info *info, int id)
{
	static const char *dml = "SELECT ctx_item, option_id FROM groupmap WHERE id = ?";
	struct context_item *item;
	sqlite3_stmt *stmt;
	const char *ctx_item;
	int option_id;
	int ret;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = sqlite3_bind_int(stmt, 1, id);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = WIDGET_ERROR_IO_ERROR;
		goto out;
	}

	ret = WIDGET_ERROR_NOT_EXIST;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ctx_item = (const char *)sqlite3_column_text(stmt, 0);
		option_id = sqlite3_column_int(stmt, 1);

		item = group_add_context_item(info, ctx_item);
		if (!item) {
			ErrPrint("Failed to add a new context item\n");
			ret = WIDGET_ERROR_FAULT;
			break;
		}

		ret = load_context_option(item, option_id);
		if (ret < 0) {
			break;
		}
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int build_group_info(struct pkg_info *info)
{
	static const char *dml = "SELECT id, cluster, category FROM groupinfo WHERE pkgid = ?";
	sqlite3_stmt *stmt;
	int ret;
	int id;
	const char *cluster_name;
	const char *category_name;
	struct cluster *cluster;
	struct category *category;
	struct context_info *ctx_info;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to bind a package name(%s)\n", package_name(info));
		sqlite3_finalize(stmt);
		return WIDGET_ERROR_IO_ERROR;
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		id = sqlite3_column_int(stmt, 0);
		cluster_name = (const char *)sqlite3_column_text(stmt, 1);
		if (!cluster_name || !strlen(cluster_name)) {
			DbgPrint("Cluster name is not valid\n");
			continue;
		}

		category_name = (const char *)sqlite3_column_text(stmt, 2);
		if (!category_name || !strlen(category_name)) {
			DbgPrint("Category name is not valid\n");
			continue;
		}

		cluster = group_find_cluster(cluster_name);
		if (!cluster) {
			cluster = group_create_cluster(cluster_name);
			if (!cluster) {
				ErrPrint("Failed to create a cluster(%s)\n", cluster_name);
				continue;
			}
		}

		category = group_find_category(cluster, category_name);
		if (!category) {
			category = group_create_category(cluster, category_name);
			if (!category) {
				ErrPrint("Failed to create a category(%s)\n", category_name);
				continue;
			}
		}

		/*!
		 * \TODO
		 * Step 1. Get the list of the context item from the DB using 'id'
		 *         {context_item, option_id}
		 * Step 2. Get the list of the options from the DB using option_id
		 *         key, value
		 */
		ctx_info = group_create_context_info(category, package_name(info));
		if (ctx_info) {
			ret = load_context_item(ctx_info, id);
			if (ret < 0) {
				if (ret == (int)WIDGET_ERROR_NOT_EXIST) {
					DbgPrint("Has no specific context info\n");
				} else {
					DbgPrint("Context info is not valid\n");
					group_destroy_context_info(ctx_info);
					ctx_info = NULL;
				}
			}

			if (ctx_info) {
				package_add_ctx_info(info, ctx_info);
			}
		}
	}

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return WIDGET_ERROR_NONE;
}

HAPI int io_is_exists(const char *lbid)
{
	sqlite3_stmt *stmt;
	int ret;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = sqlite3_prepare_v2(s_info.handle, "SELECT COUNT(pkgid) FROM pkgmap WHERE pkgid = ?", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = sqlite3_bind_text(stmt, 1, lbid, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = WIDGET_ERROR_IO_ERROR;
		goto out;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no record (%s)\n", lbid, sqlite3_errmsg(s_info.handle));
		ret = WIDGET_ERROR_IO_ERROR;
		goto out;
	}

	ret = sqlite3_column_int(stmt, 0);
out:
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

HAPI char *io_widget_pkgname(const char *pkgname)
{
	sqlite3_stmt *stmt;
	char *pkgid;
	char *tmp;
	int ret;

	pkgid = NULL;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return NULL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, "SELECT pkgid FROM pkgmap WHERE (appid = ? AND prime = 1) OR pkgid = ?", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return NULL;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgname, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, pkgname, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no record (%s)\n", pkgname, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	tmp = (char *)sqlite3_column_text(stmt, 0);
	if (tmp && strlen(tmp)) {
		pkgid = strdup(tmp);
		if (!pkgid) {
			ErrPrint("strdup: %d\n", errno);
		}
	}

out:
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);
	return pkgid;
}

HAPI int io_crawling_widgetes(int (*cb)(const char *pkgid, const char *lbid, int prime, void *data), void *data)
{
	DIR *dir;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
	} else {
		int ret;
		sqlite3_stmt *stmt;

		ret = sqlite3_prepare_v2(s_info.handle, "SELECT appid, pkgid, prime FROM pkgmap", -1, &stmt, NULL);
		if (ret != SQLITE_OK) {
			ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		} else {
			const char *lbid;
			const char *pkgid;
			int prime;

			while (sqlite3_step(stmt) == SQLITE_ROW) {
				pkgid = (const char *)sqlite3_column_text(stmt, 0);
				if (!pkgid || !strlen(pkgid)) {
					continue;
				}

				lbid = (const char *)sqlite3_column_text(stmt, 1);
				if (!lbid || !strlen(lbid)) {
					continue;
				}

				prime = (int)sqlite3_column_int(stmt, 1);

				if (cb(pkgid, lbid, prime, data) < 0) {
					sqlite3_reset(stmt);
					sqlite3_finalize(stmt);
					return WIDGET_ERROR_CANCELED;
				}
			}

			sqlite3_reset(stmt);
			sqlite3_finalize(stmt);
		}
	}

	dir = opendir(WIDGET_CONF_ROOT_PATH);
	if (!dir) {
		ErrPrint("opendir: %d\n", errno);
	} else {
		struct dirent *ent;

		while ((ent = readdir(dir))) {
			if (ent->d_name[0] == '.') {
				continue;
			}

			if (cb(ent->d_name, ent->d_name, -2, data) < 0) {
				if (closedir(dir) < 0) {
					ErrPrint("closedir: %d\n", errno);
				}
				return WIDGET_ERROR_CANCELED;
			}
		}

		if (closedir(dir) < 0) {
			ErrPrint("closedir: %d\n", errno);
		}
	}

	return WIDGET_ERROR_NONE;
}

HAPI int io_update_widget_package(const char *pkgid, int (*cb)(const char *pkgid, const char *lbid, int prime, void *data), void *data)
{
	sqlite3_stmt *stmt;
	char *lbid;
	int prime;
	int ret;

	if (!cb || !pkgid) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	ret = sqlite3_prepare_v2(s_info.handle, "SELECT pkgid, prime FROM pkgmap WHERE appid = ?", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return WIDGET_ERROR_FAULT;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = WIDGET_ERROR_FAULT;
		goto out;
	}

	ret = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		lbid = (char *)sqlite3_column_text(stmt, 0);
		if (!lbid || !strlen(lbid)) {
			continue;
		}

		prime = sqlite3_column_int(stmt, 1);

		if (cb(pkgid, lbid, prime, data) < 0) {
			DbgPrint("Callback canceled\n");
			break;
		}

		ret++;
	}
out:
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

HAPI int io_load_package_db(struct pkg_info *info)
{
	int ret;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return WIDGET_ERROR_IO_ERROR;
	}

	ret = build_provider_info(info);
	if (ret < 0) {
		return ret;
	}

	ret = build_client_info(info);
	if (ret < 0) {
		return ret;
	}

	ret = build_box_size_info(info);
	if (ret < 0) {
		return ret;
	}

	ret = build_group_info(info);
	if (ret < 0) {
		return ret;
	}

	return WIDGET_ERROR_NONE;
}

static inline int db_init(void)
{
	int ret;
	struct stat stat;

	ret = db_util_open_with_options(WIDGET_CONF_DBFILE, &s_info.handle, SQLITE_OPEN_READONLY, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to open a DB\n");
		return WIDGET_ERROR_IO_ERROR;
	}

	if (lstat(WIDGET_CONF_DBFILE, &stat) < 0) {
		ErrPrint("lstat: %d\n", errno);
		db_util_close(s_info.handle);
		s_info.handle = NULL;
		return WIDGET_ERROR_IO_ERROR;
	}

	if (!S_ISREG(stat.st_mode)) {
		ErrPrint("Invalid file\n");
		db_util_close(s_info.handle);
		s_info.handle = NULL;
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (stat.st_size <= 0) {
		DbgPrint("Size is %d (But use this ;)\n", stat.st_size);
	}

	return WIDGET_ERROR_NONE;
}

static inline int db_fini(void)
{
	if (!s_info.handle) {
		return WIDGET_ERROR_NONE;
	}

	db_util_close(s_info.handle);
	s_info.handle = NULL;

	return WIDGET_ERROR_NONE;
}

HAPI int io_init(void)
{
	int ret;

	ret = db_init();
	if (ret < 0) {
		DbgPrint("DB initialized: %d\n", ret);
	}

	return WIDGET_ERROR_NONE;
}

HAPI int io_fini(void)
{
	int ret;

	ret = db_fini();
	if (ret < 0) {
		DbgPrint("DB finalized: %d\n", ret);
	}
	return WIDGET_ERROR_NONE;
}

/* End of a file */
