#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <string.h>

#include <sqlite3.h>
#include <db-util.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <dlog.h>

#include "dlist.h"

#if !defined(FLOG)
#define DbgPrint(format, arg...)	LOGD("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg)
#define ErrPrint(format, arg...)	LOGE("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg)
#endif
/* End of a file */

/*!
 * \note
 * DB Table schema
 *
 * pkgmap
 * +-------+-------+---------+
 * | appid | pkgid | prime   |
 * +-------+-------+---------+
 * |   -   |   -   |         |
 * +-------+-------+---------+
 * CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, prime INTEGER )
 *
 *
 * provider
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+---------+---------+--------+--------+-------+
 * | pkgid | network | abi | secured | box_type | box_src | box_group | pd_type | pd_src | pd_group | libexec | timeout | period | script | pinup |
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+---------+---------+--------+--------+-------+
 * |   -   |    -    |  -  |    -    |     -    |    -    |     -     |    -    |    -   |     -    |     -   |    -    |    -   |    -   |   -   |
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+---------+---------+--------+--------+-------+
 * CREATE TABLE provider ( pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, abi TEXT, secured INTEGER, box_type INTEGER, box_src TEXT, box_group TEXT, pd_type TEXT, pd_src TEXT, pd_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid))
 *
 * = box_type = { text | buffer | script | image }
 * = pd_type = { text | buffer | script }
 * = network = { 1 | 0 }
 * = auto_launch = { 1 | 0 }
 * = secured = { 1 | 0 }
 *
 *
 * client
 * +-------+------+---------+-------------+---------+
 * | pkgid | Icon |  Name   | auto_launch | pd_size |
 * +-------+------+---------+-------------+---------+
 * |   -   |   -  |    -    |      -      |    -    |
 * +-------+------+---------+-------------+---------+
 * CREATE TABLE client ( pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, auto_launch INTEGER, pd_size TEXT,FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 * = auto_launch = { 1 | 0 }
 * = pd_size = WIDTHxHEIGHT
 *
 *
 * i18n
 * +-------+------+------+------+
 * |   fk  | lang | name | icon |
 * +-------+------+------+------+
 * | pkgid |   -  |   -  |   -  |
 * +-------+------+------+------+
 * CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT, name TEXT, icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 *
 * box_size
 * +-------+-----------+
 * | pkgid | size_type |
 * +-------+-----------+
 * |   -   |     -     |
 * +-------+-----------+
 * CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 * = box_size_list = { WIDTHxHEIGHT; WIDTHxHEIGHT; ... }
 *
 * groupinfo
 * +----+---------+----------+-------+
 * | id | cluster | category | pkgid |
 * +----+---------+----------+-------+
 * |  - |    -    |    -     |   -   |
 * +----+---------+----------+-------|
 * CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, appid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ))
 *
 * groupmap
 * +-------+----+----------+-----------+
 * | pkgid | id | ctx_item | option_id |
 * +-------+----+----------+-----------+
 * CREATE TABLE groupmap ( option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 *
 * option
 * +-------+-----------+-----+-------+
 * | pkgid | option_id | key | value |
 * +-------+-----------+-----+-------+
 * CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, FOREIGN KEY(option_id) REFERENCES groupmap(option_id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid)  )
 */

#if !defined(LIBXML_TREE_ENABLED)
	#error "LIBXML is not supporting the tree"
#endif

#if defined(LOG_TAG)
#undef LOG_TAG
#endif

#define LOG_TAG "pkgmgr_livebox"

int errno;

static struct {
	const char *dbfile;
	sqlite3 *handle;
} s_info = {
	.dbfile = "/opt/dbspace/.livebox.db",
	.handle = NULL,
};

static inline int begin_transaction(void)
{
	sqlite3_stmt *stmt;
	int ret;

	ret = sqlite3_prepare_v2(s_info.handle, "BEGIN TRANSACTION", -1, &stmt, NULL);

	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return EXIT_FAILURE;
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Failed to do update (%s)\n",
					sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return EXIT_FAILURE;
	}

	sqlite3_finalize(stmt);
	return EXIT_SUCCESS;
}

static inline int rollback_transaction(void)
{
	int ret;
	sqlite3_stmt *stmt;

	ret = sqlite3_prepare_v2(s_info.handle, "ROLLBACK TRANSACTION", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return EXIT_FAILURE;
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Failed to do update (%s)\n",
				sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return EXIT_FAILURE;
	}

	sqlite3_finalize(stmt);
	return EXIT_SUCCESS;
}

static inline int commit_transaction(void)
{
	sqlite3_stmt *stmt;
	int ret;

	ret = sqlite3_prepare_v2(s_info.handle, "COMMIT TRANSACTION", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return EXIT_FAILURE;
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Failed to do update (%s)\n",
					sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return EXIT_FAILURE;
	}

	sqlite3_finalize(stmt);
	return EXIT_SUCCESS;
}

static inline int db_create_pkgmap(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, prime INTEGER )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_pkgmap(const char *appid, const char *pkgid, int primary)
{
	int ret;
	static const char *dml;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO pkgmap ( appid, pkgid, prime ) VALUES (? ,?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, appid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 3, primary);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_pkgmap(const char *pkgid)
{
	int ret;
	static const char *dml;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM pkgmap WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_create_provider(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE provider (" \
		"pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, " \
		"abi TEXT, secured INTEGER, box_type INTEGER, " \
		"box_src TEXT, box_group TEXT, pd_type INTEGER, " \
		"pd_src TEXT, pd_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, "\
		"FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid))";

	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_remove_provider(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM provider WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_insert_provider(const char *pkgid, int net, const char *abi, int secured, int box_type, const char *box_src, const char *box_group, int pd_type, const char *pd_src, const char *pd_group, const char *libexec, const char *timeout, const char *period, const char *script, int pinup)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	if (!abi)
		abi = "c";

	if (!box_src)
		box_src = "";

	if (!box_group)
		box_group = "";

	if (!pd_src)
		pd_src = "";

	if (!pd_group)
		pd_group = "";

	if (!libexec)
		libexec = "";

	if (!timeout)
		timeout = "10";

	if (!period)
		period = "0.0";

	if (!script)
		script = "edje";

	dml = "INSERT INTO provider ( pkgid, network, abi, secured, box_type, box_src, box_group, pd_type, pd_src, pd_group, libexec, timeout, period, script, pinup ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 2, net);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, abi, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 4, secured);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 5, box_type);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 6, box_src, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 7, box_group, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 8, pd_type);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 9, pd_src, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 10, pd_group, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 11, libexec, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 12, atoi(timeout));
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 13, period, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 14, script, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 15, pinup);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_create_client(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE client (" \
		"pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, " \
		"auto_launch INTEGER, pd_size TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_client(const char *pkgid, const char *icon, const char *name, int auto_launch, const char *pd_size)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO client ( pkgid, icon, name, auto_launch, pd_size ) VALUES (?, ?, ?, ?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, icon, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, name, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 4, auto_launch);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 5, pd_size, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_client(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM client WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_create_i18n(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT, name TEXT, " \
		"icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_i18n(const char *pkgid, const char *lang, const char *name, const char *icon)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	DbgPrint("%s - lang[%s] name[%s] icon[%s]\n", pkgid, lang, name, icon);
	dml = "INSERT INTO i18n ( pkgid, lang, name, icon ) VALUES (?, ?, ?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, lang, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, name, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 4, icon, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_i18n(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM i18n WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_create_group(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, pkgid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_group(const char *pkgid, const char *cluster, const char *category)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO groupinfo ( cluster, category, pkgid ) VALUES (?, ?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, cluster, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, category, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_get_group_id(const char *cluster, const char *category)
{
	static const char *dml = "SELECT id FROM groupinfo WHERE cluster = ? AND category = ?";
	sqlite3_stmt *stmt;
	int ret;

	if (!cluster || !category) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to prepare the initial DML(%s)\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = -EIO;
	if (sqlite3_bind_text(stmt, 1, cluster, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a cluster(%s) - %s\n", cluster, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 2, category, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a category(%s) - %s\n", category, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("Failed to execute the DML for %s - %s\n", cluster, category);
		goto out;
	}

	ret = sqlite3_column_int(stmt, 0);

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_group(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM groupinfo WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_create_groupmap(void)
{
	char *err;
	static const char *ddl;

 	ddl = "CREATE TABLE groupmap (option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid))";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_get_option_id(int id, const char *pkgid, const char *ctx_item)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "SELECT option_id FROM groupmap WHERE id = ? AND pkgid = ? AND ctx_item = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_int(stmt, 1, id);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, ctx_item, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_ROW) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_column_int(stmt, 0);

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_insert_groupmap(int id, const char *pkgid, const char *ctx_item)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	DbgPrint("%d (%s) add to groupmap\n", id, pkgid);

	dml = "INSERT INTO groupmap ( id, pkgid, ctx_item ) VALUES (?, ?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_int(stmt, 1, id);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, ctx_item, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_groupmap(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM groupmap WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_create_option(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, " \
		"FOREIGN KEY(option_id) REFERENCES groupmap(option_id), " \
		"FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_option(const char *pkgid, int option_id, const char *key, const char *value)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO option (pkgid, option_id, key, value) VALUES (?, ?, ?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 2, option_id);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 3, key, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 4, value, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}
out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_option(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM option WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}


static inline int db_create_box_size(void)
{
	char *err;
	static const char *ddl;

	ddl = "CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, " \
		"FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_box_size(const char *pkgid, int size_type)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	DbgPrint("box size: %s - %d is added\n", pkgid, size_type);
	dml = "INSERT INTO box_size ( pkgid, size_type ) VALUES (?, ?)";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = sqlite3_bind_int(stmt, 2, size_type);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_box_size(const char *pkgid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM box_size WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return -EIO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgid, -1, NULL);
	if (ret != SQLITE_OK) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline void db_create_table(void)
{
	int ret;
	begin_transaction();

	ret = db_create_pkgmap();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_provider();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_client();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_i18n();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_box_size();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_group();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_option();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	ret = db_create_groupmap();
	if (ret < 0) {
		rollback_transaction();
		return;
	}

	commit_transaction();
}

static inline int db_init(void)
{
	int ret;
	struct stat stat;

	ret = db_util_open(s_info.dbfile, &s_info.handle, DB_UTIL_REGISTER_HOOK_METHOD);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to open a DB\n");
		return -EIO;
	}

	if (lstat(s_info.dbfile, &stat) < 0) {
		ErrPrint("%s\n", strerror(errno));
		db_util_close(s_info.handle);
		s_info.handle = NULL;
		return -EIO;
	}

	if (!S_ISREG(stat.st_mode)) {
		ErrPrint("Invalid file\n");
		db_util_close(s_info.handle);
		s_info.handle = NULL;
		return -EINVAL;
	}

	if (!stat.st_size)
		db_create_table();

	return 0;
}

static inline int db_fini(void)
{
	if (!s_info.handle)
		return 0;

	db_util_close(s_info.handle);
	s_info.handle = NULL;

	return 0;
}

struct i18n {
	xmlChar *lang;
	xmlChar *name;
	xmlChar *icon;
};

enum lb_type {
	LB_TYPE_NONE = 0x0,
	LB_TYPE_SCRIPT,
	LB_TYPE_FILE,
	LB_TYPE_TEXT,
	LB_TYPE_BUFFER,
};

enum pd_type {
	PD_TYPE_NONE = 0x0,
	PD_TYPE_SCRIPT,
	PD_TYPE_TEXT,
	PD_TYPE_BUFFER,
};

enum lb_size {
	LB_SIZE_1x1 = 0x01,
	LB_SIZE_2x1 = 0x02,
	LB_SIZE_2x2 = 0x04,
	LB_SIZE_4x2 = 0x08,
	LB_SIZE_4x1 = 0x10,
	LB_SIZE_4x4 = 0x20,
};

struct livebox {
	xmlChar *pkgid;
	int secured;
	int auto_launch;
	int network;
	xmlChar *abi;
	xmlChar *name; /* Default name */
	xmlChar *icon; /* Default icon */
	xmlChar *libexec; /* Path of the SO file */
	xmlChar *timeout; /* INTEGER, timeout */
	xmlChar *period; /* DOUBLE, update period */
	xmlChar *script; /* Script engine */

	int pinup; /* Is this support the pinup feature? */
	int primary; /* Is this primary livebox? */

	enum lb_type lb_type;
	xmlChar *lb_src;
	xmlChar *lb_group;
	int size_list; /* 172x172, 348x172, 348x348, 720x348 */

	enum pd_type pd_type;
	xmlChar *pd_src;
	xmlChar *pd_group;
	xmlChar *pd_size; /* Default PD size */

	struct dlist *i18n_list;
	struct dlist *group_list;
};

struct group {
	xmlChar *cluster;
	xmlChar *category;
	xmlChar *ctx_item;
	struct dlist *option_list;
};

struct option {
	xmlChar *key;
	xmlChar *value;
};

static inline int validate_pkgid(const char *appid, const char *pkgid)
{
	return !strncmp(appid, pkgid, strlen(appid));
}

static inline int livebox_destroy(struct livebox *livebox)
{
	struct dlist *l;
	struct dlist *n;
	struct i18n *i18n;
	struct group *group;
	struct option *option;
	struct dlist *il;
	struct dlist *in;

	xmlFree(livebox->pkgid);
	xmlFree(livebox->abi);
	xmlFree(livebox->name);
	xmlFree(livebox->icon);
	xmlFree(livebox->lb_src);
	xmlFree(livebox->lb_group);
	xmlFree(livebox->pd_src);
	xmlFree(livebox->pd_group);
	xmlFree(livebox->pd_size);
	xmlFree(livebox->libexec);
	xmlFree(livebox->script);

	dlist_foreach_safe(livebox->i18n_list, l, n, i18n) {
		livebox->i18n_list = dlist_remove(livebox->i18n_list, l);
		xmlFree(i18n->name);
		xmlFree(i18n->icon);
		xmlFree(i18n->lang);
		free(i18n);
	}

	dlist_foreach_safe(livebox->group_list, l, n, group) {
		livebox->group_list = dlist_remove(livebox->group_list, l);
		DbgPrint("Release %s/%s\n", group->cluster, group->category);

		if (group->ctx_item) {
			dlist_foreach_safe(group->option_list, il, in, option) {
				group->option_list = dlist_remove(group->option_list, il);
				DbgPrint("Release option %s(%s)\n", option->key, option->value);
				xmlFree(option->key);
				xmlFree(option->value);
				free(option);
			}
			xmlFree(group->ctx_item);
		}

		xmlFree(group->cluster);
		xmlFree(group->category);
		free(group);
	}

	free(livebox);
	return 0;
}

static inline void update_i18n_name(struct livebox *livebox, xmlNodePtr node)
{
	struct i18n *i18n;
	struct dlist *l;
	xmlChar *lang;
	xmlChar *name;

	name = xmlNodeGetContent(node);
	if (!name) {
		ErrPrint("Invalid tag\n");
		return;
	}

	lang = xmlNodeGetLang(node);
	if (!lang) {
		if (livebox->name) {
			DbgPrint("Override default name: %s\n", livebox->name);
			xmlFree(livebox->name);
		}

		livebox->name = name;
		return;
	}

	dlist_foreach(livebox->i18n_list, l, i18n) {
		if (!xmlStrcasecmp(i18n->lang, lang)) {
			if (i18n->name) {
				DbgPrint("Override name: %s\n", i18n->name);
				xmlFree(i18n->name);
			}

			i18n->name = name;
			return;
		}
	}

	i18n = calloc(1, sizeof(*i18n));
	if (!i18n) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree(name);
		xmlFree(lang);
		return;
	}

	i18n->name = name;
	i18n->lang = lang;
	DbgPrint("Label[%s] - [%s] added\n", i18n->lang, i18n->name);
	livebox->i18n_list = dlist_append(livebox->i18n_list, i18n);
}

static inline void update_i18n_icon(struct livebox *livebox, xmlNodePtr node)
{
	struct i18n *i18n;
	struct dlist *l;
	xmlChar *lang;
	xmlChar *icon;

	icon = xmlNodeGetContent(node);
	if (!icon) {
		ErrPrint("Invalid tag\n");
		return;
	}

	lang = xmlNodeGetLang(node);
	if (!lang) {
		if (livebox->icon) {
			DbgPrint("Override default icon: %s\n", livebox->icon);
			xmlFree(livebox->icon);
		}

		livebox->icon = icon;
		return;
	}

	dlist_foreach(livebox->i18n_list, l, i18n) {
		if (!xmlStrcasecmp(i18n->lang, lang)) {
			if (i18n->icon) {
				DbgPrint("Override icon %s for %s\n", i18n->icon, i18n->name);
				xmlFree(i18n->icon);
			}

			i18n->icon = icon;
			return;
		}
	}

	i18n = calloc(1, sizeof(*i18n));
	if (!i18n) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree(icon);
		xmlFree(lang);
		return;
	}

	i18n->icon = icon;
	i18n->lang = lang;
	DbgPrint("Icon[%s] - [%s] added\n", i18n->lang, i18n->icon);
	livebox->i18n_list = dlist_append(livebox->i18n_list, i18n);
}

static inline void update_box(struct livebox *livebox, xmlNodePtr node)
{
	if (!xmlHasProp(node, (const xmlChar *)"type")) {
		livebox->lb_type = LB_TYPE_FILE;
	} else {
		xmlChar *type;

		type = xmlGetProp(node, (const xmlChar *)"type");
		if (!type) {
			ErrPrint("Type is NIL\n");
			livebox->lb_type = LB_TYPE_FILE;
		} else {
			if (!xmlStrcasecmp(type, (const xmlChar *)"text"))
				livebox->lb_type = LB_TYPE_TEXT;
			else if (!xmlStrcasecmp(type, (const xmlChar *)"buffer"))
				livebox->lb_type = LB_TYPE_BUFFER;
			else if (!xmlStrcasecmp(type, (const xmlChar *)"script"))
				livebox->lb_type = LB_TYPE_SCRIPT;
			else /* Default */
				livebox->lb_type = LB_TYPE_FILE;

			xmlFree(type);
		}
	}

	for (node = node->children; node; node = node->next) {
		if (!xmlStrcasecmp(node->name, (const xmlChar *)"size")) {
			xmlChar *size;

			size = xmlNodeGetContent(node);
			if (!size) {
				ErrPrint("Invalid size tag\n");
				continue;
			}

			if (!xmlStrcasecmp(size, (const xmlChar *)"172x172"))
				livebox->size_list |= LB_SIZE_1x1;
			else if (!xmlStrcasecmp(size, (const xmlChar *)"348x172"))
				livebox->size_list |= LB_SIZE_2x1;
			else if (!xmlStrcasecmp(size, (const xmlChar *)"348x348"))
				livebox->size_list |= LB_SIZE_2x2;
			else if (!xmlStrcasecmp(size, (const xmlChar *)"700x348"))
				livebox->size_list |= LB_SIZE_4x2;
			else if (!xmlStrcasecmp(size, (const xmlChar *)"700x172"))
				livebox->size_list |= LB_SIZE_4x1;
			else if (!xmlStrcasecmp(size, (const xmlChar *)"700x700"))
				livebox->size_list |= LB_SIZE_4x4;
			else
				ErrPrint("Invalid size tag (%s)\n", size);

			xmlFree(size);
		} else if (!xmlStrcasecmp(node->name, (const xmlChar *)"script")) {
			xmlChar *src;

			if (!xmlHasProp(node, (const xmlChar *)"src")) {
				ErrPrint("Invalid script tag. has no src\n");
				continue;
			}

			src = xmlGetProp(node, (const xmlChar *)"src");
			if (!src) {
				ErrPrint("Invalid script tag. src is NIL\n");
				continue;
			}

			if (livebox->lb_src) {
				DbgPrint("Override lb src: %s\n", livebox->lb_src);
				xmlFree(livebox->lb_src);
			}

			livebox->lb_src = src;

			if (xmlHasProp(node, (const xmlChar *)"group")) {
				xmlChar *group;
				group = xmlGetProp(node, (const xmlChar *)"group");
				if (!group) {
					ErrPrint("Group is NIL\n");
				} else {
					if (livebox->lb_group) {
						DbgPrint("Override lb group: %s\n", livebox->lb_group);
						xmlFree(livebox->lb_group);
					}

					livebox->lb_group = group;
				}
			}
		}
	}
}

static inline void update_group(struct livebox *livebox, xmlNodePtr node)
{
	xmlNodePtr cluster;
	xmlNodePtr category;
	xmlNodePtr option_item;
	xmlChar *cluster_name;
	xmlChar *category_name;
	xmlChar *ctx_item;

	xmlChar *key;
	xmlChar *value;

	struct group *group;
	struct option *option;

	cluster = node;
	for (cluster = cluster->children; cluster; cluster = cluster->next) {
		if (xmlStrcasecmp(cluster->name, (const xmlChar *)"cluster")) {
			DbgPrint("Skip: %s\n", cluster->name);
			continue;
		}

		if (!xmlHasProp(cluster, (const xmlChar *)"name")) {
			ErrPrint("Invalid cluster, has no name\n");
			continue;
		}

		cluster_name = xmlGetProp(cluster, (const xmlChar *)"name");
		if (!cluster_name) {
			ErrPrint("Invalid cluster name. NIL\n");
			continue;
		}

		for (category = cluster->children; category; category = category->next) {
			if (xmlStrcasecmp(category->name, (const xmlChar *)"category")) {
				DbgPrint("Skip: %s\n", category->name);
				continue;
			}

			if (!xmlHasProp(category, (const xmlChar *)"name")) {
				ErrPrint("Invalid category, has no name\n");
				continue;
			}

			category_name = xmlGetProp(category, (const xmlChar *)"name");
			if (!category_name) {
				ErrPrint("Invalid category name. NIL\n");
				continue;
			}

			group = calloc(1, sizeof(*group));
			if (!group) {
				ErrPrint("Heap: %s\n", strerror(errno));
				xmlFree(category_name);
				continue;
			}

			group->cluster = xmlStrdup(cluster_name);
			if (!group->cluster) {
				ErrPrint("Heap: %s\n", strerror(errno));
				xmlFree(category_name);
				free(group);
				continue;
			}

			group->category = category_name;
			livebox->group_list = dlist_append(livebox->group_list, group);

			if (!xmlHasProp(category, (const xmlChar *)"context")) {
				DbgPrint("%s, %s has no ctx info\n", group->cluster, group->category);
				continue;
			}

			ctx_item = xmlGetProp(category, (const xmlChar *)"context");
			if (!ctx_item) {
				ErrPrint("Failed to get context ID (%s, %s)\n", group->cluster, group->category);
				continue;
			}

			group->ctx_item = ctx_item;
			DbgPrint("Build group item: %s - %s - %s\n", group->cluster, group->category, group->ctx_item);

			for (option_item = category->children; option_item; option_item = option_item->next) {
				if (xmlStrcasecmp(option_item->name, (const xmlChar *)"option")) {
					DbgPrint("Skip: %s\n", option_item->name);
					continue;
				}

				if (!xmlHasProp(option_item, (const xmlChar *)"key")) {
					ErrPrint("Invalid option, has no key\n");
					continue;
				}

				if (!xmlHasProp(option_item, (const xmlChar *)"value")) {
					ErrPrint("Invalid option, has no value\n");
					continue;
				}

				key = xmlGetProp(option_item, (const xmlChar *)"key");
				if (!key) {
					ErrPrint("Invalid key. NIL\n");
					continue;
				}

				value = xmlGetProp(option_item, (const xmlChar *)"value");
				if (!value) {
					ErrPrint("Invalid valid. NIL\n");
					xmlFree(key);
					continue;
				}

				option = calloc(1, sizeof(*option));
				if (!option) {
					ErrPrint("Heap: %s\n", strerror(errno));
					xmlFree(key);
					xmlFree(value);
					continue;
				}

				option->key = key;
				option->value = value;

				group->option_list = dlist_append(group->option_list, option);
			}
		}

		xmlFree(cluster_name);
	}
}

static inline void update_pd(struct livebox *livebox, xmlNodePtr node)
{
	if (!xmlHasProp(node, (const xmlChar *)"type")) {
		livebox->pd_type = PD_TYPE_SCRIPT;
	} else {
		xmlChar *type;

		type = xmlGetProp(node, (const xmlChar *)"type");
		if (!type) {
			ErrPrint("type is NIL\n");
			return;
		}

		if (!xmlStrcasecmp(type, (const xmlChar *)"text"))
			livebox->pd_type = PD_TYPE_TEXT;
		else if (!xmlStrcasecmp(type, (const xmlChar *)"buffer"))
			livebox->pd_type = PD_TYPE_BUFFER;
		else
			livebox->pd_type = PD_TYPE_SCRIPT;

		xmlFree(type);
	}

	for (node = node->children; node; node = node->next) {
		if (!xmlStrcasecmp(node->name, (const xmlChar *)"size")) {
			xmlChar *size;

			size = xmlNodeGetContent(node);
			if (!size) {
				ErrPrint("Invalid size tag\n");
				continue;
			}

			if (livebox->pd_size) {
				DbgPrint("Override pd size: %s\n", livebox->pd_size);
				xmlFree(livebox->pd_size);
			}
			livebox->pd_size = size;
		} else if (!xmlStrcasecmp(node->name, (const xmlChar *)"script")) {
			xmlChar *src;

			if (!xmlHasProp(node, (const xmlChar *)"src")) {
				ErrPrint("Invalid script tag, has no src\n");
				continue;
			}

			src = xmlGetProp(node, (const xmlChar *)"src");
			if (!src) {
				ErrPrint("src is NIL\n");
				continue;
			}

			if (livebox->pd_src) {
				DbgPrint("Overide PD src: %s\n", livebox->pd_src);
				xmlFree(livebox->pd_src);
			}

			livebox->pd_src = src;

			if (xmlHasProp(node, (const xmlChar *)"group")) {
				xmlChar *group;
				group = xmlGetProp(node, (const xmlChar *)"group");
				if (!group) {
					ErrPrint("Group is NIL\n");
				} else {
					if (livebox->pd_group) {
						DbgPrint("Override PD group : %s\n", livebox->pd_group);
						xmlFree(livebox->pd_group);
					}

					livebox->pd_group = group;
				}
			}
		}
	}
}

static inline int db_insert_livebox(struct livebox *livebox, const char *appid)
{
	struct dlist *l;
	struct dlist *il;
	struct i18n *i18n;
	struct group *group;
	int ret;
	int id;
	struct option *option;

	begin_transaction();
	ret = db_insert_pkgmap(appid, (char *)livebox->pkgid, livebox->primary);
	if (ret < 0)
		goto errout;

	ret = db_insert_provider((char *)livebox->pkgid, livebox->network, (char *)livebox->abi, livebox->secured, livebox->lb_type, (char *)livebox->lb_src, (char *)livebox->lb_group, livebox->pd_type, (char *)livebox->pd_src, (char *)livebox->pd_group, (char *)livebox->libexec, (char *)livebox->timeout, (char *)livebox->period, (char *)livebox->script, livebox->pinup);
	if (ret < 0)
		goto errout;

	ret = db_insert_client((char *)livebox->pkgid, (char *)livebox->icon, (char *)livebox->name, livebox->auto_launch, (char *)livebox->pd_size);
	if (ret < 0)
		goto errout;

	dlist_foreach(livebox->i18n_list, l, i18n) {
		ret = db_insert_i18n((char *)livebox->pkgid, (char *)i18n->lang, (char *)i18n->name, (char *)i18n->icon);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_1x1) {
		ret = db_insert_box_size((char *)livebox->pkgid, LB_SIZE_1x1);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_2x1) {
		ret = db_insert_box_size((char *)livebox->pkgid, LB_SIZE_2x1);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_2x2) {
		ret = db_insert_box_size((char *)livebox->pkgid, LB_SIZE_2x2);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_4x2) {
		ret = db_insert_box_size((char *)livebox->pkgid, LB_SIZE_4x2);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_4x1) {
		ret = db_insert_box_size((char *)livebox->pkgid, LB_SIZE_4x1);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_4x4) {
		ret = db_insert_box_size((char *)livebox->pkgid, LB_SIZE_4x4);
		if (ret < 0)
			goto errout;
	}

	dlist_foreach(livebox->group_list, l, group) {
		/* group ID "id" */
		id = db_get_group_id((char *)group->cluster, (char *)group->category);
		if (id < 0) {
			int ret;
			
			ret = db_insert_group((char *)livebox->pkgid, (char *)group->cluster, (char *)group->category);
			if (ret < 0) {
				ErrPrint("[%s]-[%s] is not exists\n", group->cluster, group->category);
				continue;
			}

			DbgPrint("New group name is built - %s/%s\n", group->cluster, group->category);
			id = db_get_group_id((char *)group->cluster, (char *)group->category);
			if (id < 0) {
				ErrPrint("Failed to get group id for %s/%s\n", group->cluster, group->category);
				continue;
			}
		}

		if (!group->ctx_item) {
			DbgPrint("%s, %s - has no ctx info\n", group->cluster, group->category);
			continue;
		}

		ret = db_insert_groupmap(id, (char *)livebox->pkgid, (char *)group->ctx_item);
		if (ret < 0)
			goto errout;

		/* REUSE "id" from here , option ID */
		id = db_get_option_id(id, (char *)livebox->pkgid, (char *)group->ctx_item);
		if (id < 0)
			goto errout;

		dlist_foreach(group->option_list, il, option) {
			ret = db_insert_option((char *)livebox->pkgid, id, (char *)option->key, (char *)option->value);
			if (ret < 0)
				goto errout;
		}
	}

	commit_transaction();
	livebox_destroy(livebox);
	return 0;

errout:
	ErrPrint("ROLLBACK\n");
	rollback_transaction();
	livebox_destroy(livebox);
	return ret;
}

int PKGMGR_PARSER_PLUGIN_INSTALL(xmlDocPtr docPtr, const char *appid)
{
	xmlNodePtr node;
	struct livebox *livebox;
	xmlChar *pkgid;
	xmlChar *tmp;

	if (!s_info.handle) {
		if (db_init() < 0) {
			ErrPrint("Failed to init DB\n");
			return -EIO;
		}
	}

	node = xmlDocGetRootElement(docPtr);
	if (!node) {
		ErrPrint("Invalid document\n");
		return -EINVAL;
	}

	for (node = node->children; node; node = node->next) {
		if (!xmlStrcasecmp(node->name, (const xmlChar *)"livebox"))
			break;
	}

	if (!node) {
		ErrPrint("Root has no children\n");
		return -EINVAL;
	}

	if (!xmlHasProp(node, (const xmlChar *)"appid")) {
		ErrPrint("Missing appid\n");
		return -EINVAL;
	}

	pkgid = xmlGetProp(node, (const xmlChar *)"appid");
	if (!pkgid || !validate_pkgid(appid, (char *)pkgid)) {
		ErrPrint("Invalid appid\n");
		xmlFree(pkgid);
		return -EINVAL;
	}

	livebox = calloc(1, sizeof(*livebox));
	if (!livebox) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree(pkgid);
		return -ENOMEM;
	}

	livebox->pkgid = pkgid;

	if (xmlHasProp(node, (const xmlChar *)"primary")) {
		tmp = xmlGetProp(node, (const xmlChar *)"primary");
		livebox->primary = !xmlStrcasecmp(tmp, (const xmlChar *)"true");
		xmlFree(tmp);
	}

	if (xmlHasProp(node, (const xmlChar *)"script")) {
		livebox->script = xmlGetProp(node, (const xmlChar *)"script");
		if (!livebox->script)
			ErrPrint("script is NIL\n");
	}

	if (xmlHasProp(node, (const xmlChar *)"pinup")) {
		tmp = xmlGetProp(node, (const xmlChar *)"pinup");
		livebox->pinup = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
		xmlFree(tmp);
	}

	if (xmlHasProp(node, (const xmlChar *)"secured")) {
		tmp = xmlGetProp(node, (const xmlChar *)"secured");
		livebox->secured = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
		xmlFree(tmp);
	}

	if (xmlHasProp(node, (const xmlChar *)"auto_launch")) {
		tmp = xmlGetProp(node, (const xmlChar *)"auto_launch");
		livebox->auto_launch = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
		xmlFree(tmp);
	} else {
		livebox->auto_launch = 1; /* Default value */
	}

	if (xmlHasProp(node, (const xmlChar *)"network")) {
		tmp = xmlGetProp(node, (const xmlChar *)"network");
		livebox->network = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
		xmlFree(tmp);
	}

	if (xmlHasProp(node, (const xmlChar *)"abi")) {
		livebox->abi = xmlGetProp(node, (const xmlChar *)"abi");
		if (!livebox->abi) {
			ErrPrint("ABI is NIL\n");
			livebox_destroy(livebox);
			return -EFAULT;
		}
	} else {
		livebox->abi = xmlStrdup((const xmlChar *)"c");
		if (!livebox->abi) {
			ErrPrint("Heap: %s\n", strerror(errno));
			livebox_destroy(livebox);
			return -ENOMEM;
		}
	}

	if (xmlHasProp(node, (const xmlChar *)"libexec")) {
		livebox->libexec = xmlGetProp(node, (const xmlChar *)"libexec");
		if (!livebox->libexec) {
			ErrPrint("libexec is NIL\n");
			livebox_destroy(livebox);
			return -EFAULT;
		}
	} else if (!xmlStrcasecmp(livebox->abi, (const xmlChar *)"c") || !xmlStrcasecmp(livebox->abi, (const xmlChar *)"cpp")) {
		char *filename;
		int len;

		len = strlen((char *)livebox->pkgid) + strlen("/libexec/liblive-.so") + 1;

		filename = malloc(len);
		if (!filename) {
			livebox_destroy(livebox);
			return -ENOMEM;
		}

		snprintf(filename, len, "/libexec/liblive-%s.so", livebox->pkgid);
		livebox->libexec = xmlStrdup((xmlChar *)filename);
		free(filename);

		if (!livebox->libexec) {
			livebox_destroy(livebox);
			return -ENOMEM;
		}

		DbgPrint("Use the default libexec: %s\n", filename);
	}

	for (node = node->children; node; node = node->next) {
		if (!xmlStrcmp(node->name, (const xmlChar *)"text"))
			continue;

		DbgPrint("Nodename: %s\n", node->name);
		if (!xmlStrcasecmp(node->name, (const xmlChar *)"label")) {
			update_i18n_name(livebox, node);
			continue;
		}

		if (!xmlStrcasecmp(node->name, (const xmlChar *)"icon")) {
			update_i18n_icon(livebox, node);
			continue;
		}

		if (!xmlStrcasecmp(node->name, (const xmlChar *)"box")) {
			update_box(livebox, node);
			continue;
		}

		if (!xmlStrcasecmp(node->name, (const xmlChar *)"pd")) {
			update_pd(livebox, node);
			continue;
		}

		if (!xmlStrcasecmp(node->name, (const xmlChar *)"group")) {
			update_group(livebox, node);
			continue;
		}
	}

	return db_insert_livebox(livebox, appid);
}

int PKGMGR_PARSER_PLUGIN_UPGRADE(xmlDocPtr docPtr, const char *appid)
{
	xmlNodePtr node;

	if (!s_info.handle) {
		if (db_init() < 0) {
			ErrPrint("Failed to init DB\n");
			return -EIO;
		}
	}

	node = xmlDocGetRootElement(docPtr);
	if (!node) {
		ErrPrint("Invalid document\n");
		return -EINVAL;
	}

	for (node = node->children; node; node = node->next) {
		if (!xmlStrcasecmp(node->name, (const xmlChar *)"livebox"))
			break;
	}

	if (!node) {
		ErrPrint("Root has no livebox\n");
		return -EINVAL;
	}

	return 0;
}

int PKGMGR_PARSER_PLUGIN_UNINSTALL(xmlDocPtr docPtr, const char *appid)
{
	xmlNodePtr node;
	xmlChar *pkgid;
	int ret;

	if (!s_info.handle) {
		if (db_init() < 0) {
			ErrPrint("Failed to init DB\n");
			return -EIO;
		}
	}

	node = xmlDocGetRootElement(docPtr);
	if (!node) {
		ErrPrint("Invalid document\n");
		return -EINVAL;
	}

	for (node = node->children; node; node = node->next) {
		if (!xmlStrcasecmp(node->name, (const xmlChar *)"livebox"))
			break;
	}

	if (!node) {
		ErrPrint("Root has no livebox\n");
		return -EINVAL;
	}

	if (!xmlHasProp(node, (const xmlChar *)"appid")) {
		ErrPrint("Missing appid\n");
		return -EINVAL;
	}

	pkgid = xmlGetProp(node, (const xmlChar *)"appid");
	if (!validate_pkgid(appid, (char *)pkgid)) {
		ErrPrint("Invalid package\n");
		xmlFree(pkgid);
		return -EINVAL;
	}

	begin_transaction();
	ret = db_remove_box_size((char *)pkgid);
	if (ret < 0)
		goto errout;

	ret = db_remove_i18n((char *)pkgid);
	if (ret < 0)
		goto errout;

	ret = db_remove_client((char *)pkgid);
	if (ret < 0)
		goto errout;

	ret = db_remove_provider((char *)pkgid);
	if (ret < 0)
		goto errout;

	ret = db_remove_option((char *)pkgid);
	DbgPrint("Remove option: %d\n", ret);

	ret = db_remove_groupmap((char *)pkgid);
	DbgPrint("Remove groupmap: %d\n", ret);

	ret = db_remove_group((char *)pkgid);
	if (ret < 0)
		goto errout;

	ret = db_remove_pkgmap((char *)pkgid);
	if (ret < 0)
		goto errout;

	commit_transaction();
	xmlFree(pkgid);
	return 0;

errout:
	rollback_transaction();
	xmlFree(pkgid);
	return ret;
}

/*
int main(int argc, char *argv[])
{
	xmlDoc *doc;
	xmlNode *root;

	if (argc != 2) {
		ErrPRint("Invalid argument: %s XML_FILENAME\n", argv[0]);
		return -EINVAL;
	}

	doc = xmlReadFile(argv[1], NULL, 0);
	if (!doc) {
		ErrPrint("Failed to parse %s\n", argv[1]);
		return -EIO;
	}

	root = xmlDocGetRootElement(doc);

	db_init();
	install_shortcut("", root);
	db_fini();

	xmlFreeDoc(doc);
	xmlCleanupParser();
	return 0;
}
*/

/* End of a file */
