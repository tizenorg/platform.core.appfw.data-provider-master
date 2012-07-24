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
 * +-------+-------+
 * | appid | pkgid |
 * +-------+-------+
 * |   -   |   -   |
 * +-------+-------+
 * CREATE TABLE pkgmap ( appid TEXT PRIMARY KEY, pkgid TEXT )
 *
 *
 * provider
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+
 * | appid | network | abi | secured | box_type | box_src | box_group | pd_type | pd_src | pd_group |
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+
 * |   -   |    -    |  -  |    -    |     -    |    -    |     -     |    -    |    -   |     -    |
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+
 * CREATE TABLE provider ( appid TEXT PRIMARY KEY NOT NULL, network INTEGER, abi TEXT, secured INTEGER, box_type INTEGER, box_src TEXT, box_group TEXT, pd_type TEXT, pd_src TEXT, pd_group TEXT, FOREIGN KEY(appid) REFERENCES pkgmap(appid))
 *
 * = box_type = { text | buffer | script | image }
 * = pd_type = { text | buffer | script }
 * = abi = { c | cpp | html }
 * = network = { 1 | 0 }
 * = auto_launch = { 1 | 0 }
 * = secured = { 1 | 0 }
 *
 *
 * client
 * +-------+------+---------+-------------+---------+
 * | appid | Icon |  Name   | auto_launch | pd_size |
 * +-------+------+---------+-------------+---------+
 * |   -   |   -  |    -    |      -      |    -    |
 * +-------+------+---------+-------------+---------+
 * CREATE TABLE client ( appid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, auto_launch INTEGER, pd_size TEXT,FOREIGN KEY(appid) REFERENCES pkgmap(appid) )
 *
 * = auto_launch = { 1 | 0 }
 * = pd_size = WIDTHxHEIGHT
 *
 *
 * i18n
 * +-------+------+------+------+
 * |   fk  | lang | name | icon |
 * +-------+------+------+------+
 * | appid |   -  |   -  |   -  |
 * +-------+------+------+------+
 * CREATE TABLE i18n ( appid TEXT NOT NULL, lang TEXT, name TEXT, icon TEXT, FOREIGN KEY(appid) REFERENCES pkgmap(appid) )
 *
 *
 * box_size
 * +-------+-----------+
 * | appid | size_type |
 * +-------+-----------+
 * |   -   |     -     |
 * +-------+-----------+
 * CREATE TABLE box_size ( appid TEXT NOT NULL, size_type INTEGER, FOREIGN KEY(appid) REFERENCES pkgmap(appid) )
 *
 * = box_size_list = { WIDTHxHEIGHT; WIDTHxHEIGHT; ... }
 *
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

	ddl = "CREATE TABLE pkgmap ( appid TEXT PRIMARY KEY, pkgid TEXT )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_pkgmap(const char *appid, const char *pkgid)
{
	int ret;
	static const char *dml;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO pkgmap ( appid, pkgid ) VALUES (? ,?)";
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

static inline int db_remove_pkgmap(const char *appid)
{
	int ret;
	static const char *dml;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM pkgmap WHERE appid = ?";
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
		"appid TEXT PRIMARY KEY NOT NULL, network INTEGER, " \
		"abi TEXT, secured INTEGER, box_type INTEGER, " \
		"box_src TEXT, box_group TEXT, pd_type INTEGER, " \
		"pd_src TEXT, pd_group TEXT, FOREIGN KEY(appid) REFERENCES pkgmap(appid))";

	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_remove_provider(const char *appid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM provider WHERE appid = ?";
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

static inline int db_insert_provider(const char *appid, int net, const char *abi, int secured, int box_type, const char *box_src, const char *box_group, int pd_type, const char *pd_src, const char *pd_group)
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

	dml = "INSERT INTO provider ( appid, network, abi, secured, box_type, box_src, box_group, pd_type, pd_src, pd_group ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
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
		"appid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, " \
		"auto_launch INTEGER, pd_size TEXT, FOREIGN KEY(appid) REFERENCES pkgmap(appid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_client(const char *appid, const char *icon, const char *name, int auto_launch, const char *pd_size)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO client ( appid, icon, name, auto_launch, pd_size ) VALUES (?, ?, ?, ?, ?)";
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

static inline int db_remove_client(const char *appid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM client WHERE appid = ?";
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

	ddl = "CREATE TABLE i18n ( appid TEXT NOT NULL, lang TEXT, name TEXT, " \
		"icon TEXT, FOREIGN KEY(appid) REFERENCES pkgmap(appid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_i18n(const char *appid, const char *lang, const char *name, const char *icon)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO i18n ( appid, lang, name, icon ) VALUES (?, ?, ?, ?)";
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

static inline int db_remove_i18n(const char *appid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM i18n WHERE appid = ?";
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

	ddl = "CREATE TABLE box_size ( appid TEXT NOT NULL, size_type INTEGER, " \
		"FOREIGN KEY(appid) REFERENCES pkgmap(appid) )";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return -EIO;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	return 0;
}

static inline int db_insert_box_size(const char *appid, int size_type)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "INSERT INTO box_size ( appid, size_type ) VALUES (?, ?)";
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

static inline int db_remove_box_size(const char *appid)
{
	static const char *dml;
	int ret;
	sqlite3_stmt *stmt;

	dml = "DELETE FROM box_size WHERE appid = ?";
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
		return -EIO;
	}

	if (!S_ISREG(stat.st_mode)) {
		ErrPrint("Invalid file\n");
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
	char *lang;
	char *name;
	char *icon;
};

enum lb_type {
	LB_TYPE_IMAGE = 0x00,
	LB_TYPE_SCRIPT = 0x01,
	LB_TYPE_BUFFER = 0x02,
	LB_TYPE_TEXT = 0x04,
	LB_TYPE_UNKNOWN,
};

enum pd_type {
	PD_TYPE_SCRIPT = 0x00,
	PD_TYPE_BUFFER = 0x01,
	PD_TYPE_TEXT = 0x02,
	PD_TYPE_UNKNOWN,
};

enum lb_size {
	LB_SIZE_1x1 = 0x01,
	LB_SIZE_2x1 = 0x02,
	LB_SIZE_2x2 = 0x04,
	LB_SIZE_4x2 = 0x08,
};

struct livebox {
	char *appid;
	int secured;
	int auto_launch;
	int network;
	char *abi;
	char *name; /* Default name */
	char *icon; /* Default icon */

	enum lb_type lb_type;
	char *lb_src;
	char *lb_group;
	int size_list; /* 172x172, 348x172, 348x348, 720x348 */

	enum pd_type pd_type;
	char *pd_src;
	char *pd_group;
	char *pd_size; /* Default PD size */

	struct dlist *i18n_list;
};

static inline int validate_abi(const char *abi)
{
	return !strcasecmp(abi, "c") || !strcasecmp(abi, "cpp") || !strcasecmp(abi, "html");
}

static inline int validate_appid(const char *pkgname, const char *appid)
{
	return !strncmp(pkgname, appid, strlen(pkgname));
}

static inline int livebox_destroy(struct livebox *livebox)
{
	struct dlist *l;
	struct dlist *n;
	struct i18n *i18n;

	free(livebox->appid);
	free(livebox->abi);
	free(livebox->name);
	free(livebox->icon);
	free(livebox->lb_src);
	free(livebox->lb_group);
	free(livebox->pd_src);
	free(livebox->pd_group);
	free(livebox->pd_size);

	dlist_foreach_safe(livebox->i18n_list, l, n, i18n) {
		livebox->i18n_list = dlist_remove(livebox->i18n_list, l);
		free(i18n->name);
		free(i18n->icon);
		free(i18n->lang);
		free(i18n);
	}

	return 0;
}

static inline void update_i18n_name(struct livebox *livebox, xmlNodePtr node)
{
	struct i18n *i18n;
	struct dlist *l;
	char *lang;
	char *name;

	if (!xmlHasProp(node, (xmlChar *)"xml:lang")) {
		char *org;
		char *tmp;

		org = livebox->name;
		if (org)
			DbgPrint("Override default name: %s\n", org);

		tmp = (char *)xmlNodeGetContent(node);
		livebox->name = strdup(tmp);
		if (!livebox->name) {
			ErrPrint("Heap: %s\n", strerror(errno));
			livebox->name = org;
		} else {
			free(org);
		}
		xmlFree((xmlChar *)tmp);

		return;
	}

	name = (char *)xmlNodeGetContent(node);
	if (!name) {
		ErrPrint("Invalid tag\n");
		return;
	}
	lang = (char *)xmlGetProp(node, (xmlChar *)"xml:lang");

	dlist_foreach(livebox->i18n_list, l, i18n) {
		if (!strcmp(i18n->lang, lang)) {
			char *org;

			org = i18n->name;
			if (org)
				DbgPrint("Override name: %s\n", org);

			i18n->name = strdup(name);
			if (!i18n->name) {
				ErrPrint("Heap: %s (%s)\n", strerror(errno), name);
				i18n->name = org;
			} else {
				free(org);
			}

			xmlFree((xmlChar *)name);
			xmlFree((xmlChar *)lang);
			return;
		}
	}

	i18n = calloc(1, sizeof(*i18n));
	if (!i18n) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)name);
		xmlFree((xmlChar *)lang);
		return;
	}

	i18n->name = strdup(name);
	if (!i18n->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)name);
		xmlFree((xmlChar *)lang);
		free(i18n);
		return;
	}

	xmlFree((xmlChar *)name);

	i18n->lang = strdup(lang);
	if (!i18n->lang) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)lang);
		free(i18n->name);
		free(i18n);
		return;
	}

	xmlFree((xmlChar *)lang);
	DbgPrint("Label[%s] - [%s] added\n", i18n->lang, i18n->name);
}

static inline void update_i18n_icon(struct livebox *livebox, xmlNodePtr node)
{
	struct i18n *i18n;
	struct dlist *l;
	char *lang;
	char *icon;

	if (!xmlHasProp(node, (xmlChar *)"xml:lang")) {
		char *org;
		char *tmp;

		org = livebox->icon;
		if (org)
			DbgPrint("Override default icon: %s\n", org);

		tmp = (char *)xmlNodeGetContent(node);
		livebox->icon = strdup(tmp);
		if (!livebox->icon) {
			ErrPrint("Heap: %s\n", strerror(errno));
			livebox->icon = org;
		} else {
			free(org);
		}
		xmlFree((xmlChar *)tmp);

		return;
	}

	icon = (char *)xmlNodeGetContent(node);
	if (!icon) {
		ErrPrint("Invalid tag\n");
		return;
	}
	lang = (char *)xmlGetProp(node, (xmlChar *)"xml:lang");

	dlist_foreach(livebox->i18n_list, l, i18n) {
		if (!strcmp(i18n->lang, lang)) {
			char *org;

			org = i18n->icon;
			if (org)
				DbgPrint("Override icon: %s\n", org);

			i18n->icon = strdup(icon);
			if (!i18n->icon) {
				ErrPrint("Heap: %s (%s)\n", strerror(errno), icon);
				i18n->icon = org;
			} else {
				free(org);
			}

			xmlFree((xmlChar *)icon);
			xmlFree((xmlChar *)lang);
			return;
		}
	}

	i18n = calloc(1, sizeof(*i18n));
	if (!i18n) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)icon);
		xmlFree((xmlChar *)lang);
		return;
	}

	i18n->icon = strdup(icon);
	if (!i18n->icon) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)icon);
		xmlFree((xmlChar *)lang);
		free(i18n);
		return;
	}

	xmlFree((xmlChar *)icon);

	i18n->lang = strdup(lang);
	if (!i18n->lang) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)lang);
		free(i18n->icon);
		free(i18n);
		return;
	}

	xmlFree((xmlChar *)lang);

	DbgPrint("Icon[%s] - [%s] added\n", i18n->lang, i18n->icon);
}

static inline void update_box(struct livebox *livebox, xmlNodePtr node)
{
	if (!xmlHasProp(node, (xmlChar *)"type")) {
		livebox->lb_type = LB_TYPE_IMAGE;
	} else {
		char *type;
		type = (char *)xmlGetProp(node, (xmlChar *)"type");

		if (!strcmp(type, "text"))
			livebox->lb_type = LB_TYPE_TEXT;
		else if (!strcmp(type, "buffer"))
			livebox->lb_type = LB_TYPE_BUFFER;
		else if (!strcmp(type, "script"))
			livebox->lb_type = LB_TYPE_SCRIPT;
		else /* Default */
			livebox->lb_type = LB_TYPE_IMAGE;

		xmlFree((xmlChar *)type);
	}

	for (node = node->children; node; node = node->next) {
		if (!strcmp((char *)node->name, "size")) {
			char *size;

			size = (char *)xmlNodeGetContent(node);
			if (!size) {
				ErrPrint("Invalid size tag\n");
				continue;
			}

			if (!strcmp(size, "172x172"))
				livebox->size_list |= LB_SIZE_1x1;
			else if (!strcmp(size, "348x172"))
				livebox->size_list |= LB_SIZE_2x1;
			else if (!strcmp(size, "348x348"))
				livebox->size_list |= LB_SIZE_2x2;
			else if (!strcmp(size, "700x348"))
				livebox->size_list |= LB_SIZE_4x2;
			else
				ErrPrint("Invalid size tag (%s)\n", size);

			xmlFree((xmlChar *)size);
			continue;
		}

		if (!strcmp((char *)node->name, "script")) {
			if (!strcmp(livebox->abi, "html")) {
				char *src;
				char *org;

				if (!xmlHasProp(node, (xmlChar *)"src")) {
					ErrPrint("Invalid script tag. has no src\n");
					continue;
				}

				src = (char *)xmlGetProp(node, (xmlChar *)"src");

				org = livebox->lb_src;
				if (org)
					DbgPrint("Override LB src: %s\n", org);

				livebox->lb_src = strdup(src);
				if (!livebox->lb_src) {
					ErrPrint("Heap: %s\n", strerror(errno));
					livebox->lb_src = org;
				} else {
					free(org);
				}

				xmlFree((xmlChar *)src);
			} else if (livebox->lb_type == LB_TYPE_SCRIPT) {
				char *src;
				char *group;

				char *org_src;
				char *org_group;

				if (!xmlHasProp(node, (xmlChar *)"src")) {
					ErrPrint("Invalid script tag. has no src\n");
					continue;
				}

				if (!xmlHasProp(node, (xmlChar *)"group")) {
					ErrPrint("Invalid script tag. has no group\n");
					continue;
				}

				src = (char *)xmlGetProp(node, (xmlChar *)"src");
				group = (char *)xmlGetProp(node, (xmlChar *)"group");

				org_src = livebox->lb_src;
				org_group = livebox->lb_group;
				if (org_src || org_group)
					DbgPrint("Override LB src & group: %s - %s\n", org_src, org_group);

				livebox->lb_src = strdup(src);
				livebox->lb_group = strdup(group);
				if (!livebox->lb_src || !livebox->lb_group) {
					ErrPrint("Heap: %s\n", strerror(errno));
					free(livebox->lb_src);
					free(livebox->lb_group);
					livebox->lb_src = org_src;
					livebox->lb_group = org_group;
				} else {
					free(org_src);
					free(org_group);
				}

				xmlFree((xmlChar *)src);
				xmlFree((xmlChar *)group);
			} else {
				ErrPrint("Invalid script tag\n");
			}

			continue;
		}
	}
}

static inline void update_pd(struct livebox *livebox, xmlNodePtr node)
{
	if (!xmlHasProp(node, (xmlChar *)"type")) {
		livebox->pd_type = PD_TYPE_SCRIPT;
	} else {
		char *type;
		type = (char *)xmlGetProp(node, (xmlChar *)"type");

		if (!strcmp(type, "text"))
			livebox->pd_type = PD_TYPE_TEXT;
		else if (!strcmp(type, "buffer"))
			livebox->pd_type = PD_TYPE_BUFFER;
		else
			livebox->pd_type = PD_TYPE_SCRIPT;

		xmlFree((xmlChar *)type);
	}

	for (node = node->children; node; node = node->next) {
		if (!strcmp((char *)node->name, "size")) {
			char *size;
			char *org;

			size = (char *)xmlNodeGetContent(node);
			if (!size) {
				ErrPrint("Invalid size tag\n");
				continue;
			}

			org = livebox->pd_size;
			if (org)
				DbgPrint("Override pd size: %s\n", org);

			livebox->pd_size = strdup(size);
			if (!livebox->pd_size) {
				ErrPrint("Heap: %s\n", strerror(errno));
				livebox->pd_size = org;
			} else {
				free(org);
			}

			xmlFree((xmlChar *)size);
			continue;
		}

		if (!strcmp((char *)node->name, "script")) {
			if (!strcmp(livebox->abi, "html")) {
				char *src;
				char *org;

				if (!xmlHasProp(node, (xmlChar *)"src")) {
					ErrPrint("Invalid script tag. has no src\n");
					continue;
				}

				src = (char *)xmlGetProp(node, (xmlChar *)"src");

				org = livebox->pd_src;
				if (org)
					DbgPrint("Override LB src: %s\n", org);

				livebox->pd_src = strdup(src);
				if (!livebox->pd_src) {
					ErrPrint("Heap: %s\n", strerror(errno));
					livebox->pd_src = org;
				} else {
					free(org);
				}

				xmlFree((xmlChar *)src);
			} else if (livebox->lb_type == LB_TYPE_SCRIPT) {
				char *src;
				char *group;

				char *org_src;
				char *org_group;

				if (!xmlHasProp(node, (xmlChar *)"src")) {
					ErrPrint("Invalid script tag. has no src\n");
					continue;
				}

				if (!xmlHasProp(node, (xmlChar *)"group")) {
					ErrPrint("Invalid script tag. has no group\n");
					continue;
				}

				src = (char *)xmlGetProp(node, (xmlChar *)"src");
				group = (char *)xmlGetProp(node, (xmlChar *)"group");

				org_src = livebox->pd_src;
				org_group = livebox->pd_group;
				if (org_src || org_group)
					DbgPrint("Override LB src & group: %s - %s\n", org_src, org_group);

				livebox->pd_src = strdup(src);
				livebox->pd_group = strdup(group);
				if (!livebox->pd_src || !livebox->pd_group) {
					ErrPrint("Heap: %s\n", strerror(errno));
					free(livebox->pd_src);
					free(livebox->pd_group);
					livebox->pd_src = org_src;
					livebox->pd_group = org_group;
				} else {
					free(org_src);
					free(org_group);
				}

				xmlFree((xmlChar *)src);
				xmlFree((xmlChar *)group);
			} else {
				ErrPrint("Invalid script tag\n");
			}

			continue;
		}
	}
}

static inline int db_insert_livebox(struct livebox *livebox, const char *pkgname)
{
	struct dlist *l;
	struct i18n *i18n;
	int ret;

	begin_transaction();
	ret = db_insert_pkgmap(livebox->appid, pkgname);
	if (ret < 0)
		goto errout;

	ret = db_insert_provider(livebox->appid, livebox->network, livebox->abi, livebox->secured, livebox->lb_type, livebox->lb_src, livebox->lb_group, livebox->pd_type, livebox->pd_src, livebox->pd_group);
	if (ret < 0)
		goto errout;

	ret = db_insert_client(livebox->appid, livebox->icon, livebox->name, livebox->auto_launch, livebox->pd_size);
	if (ret < 0)
		goto errout;

	dlist_foreach(livebox->i18n_list, l, i18n) {
		ret = db_insert_i18n(livebox->appid, i18n->lang, i18n->name, i18n->icon);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_1x1) {
		ret = db_insert_box_size(livebox->appid, LB_SIZE_1x1);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_2x1) {
		ret = db_insert_box_size(livebox->appid, LB_SIZE_2x1);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_2x2) {
		ret = db_insert_box_size(livebox->appid, LB_SIZE_2x2);
		if (ret < 0)
			goto errout;
	}

	if (livebox->size_list & LB_SIZE_4x2) {
		ret = db_insert_box_size(livebox->appid, LB_SIZE_4x2);
		if (ret < 0)
			goto errout;
	}

	commit_transaction();
	livebox_destroy(livebox);
	return 0;

errout:
	rollback_transaction();
	livebox_destroy(livebox);
	return ret;
}

int PKGMGR_PARSER_PLUGIN_INSTALL(xmlDocPtr docPtr, const char *pkgname)
{
	xmlNodePtr node;
	struct livebox *livebox;
	char *appid;
	char *tmp;

	if (!s_info.handle)
		db_init();

	if (!s_info.handle) {
		ErrPrint("Failed to init DB\n");
		return -EIO;
	}

	node = xmlDocGetRootElement(docPtr);
	if (!node) {
		ErrPrint("Invalid document\n");
		return -EINVAL;
	}

	if (strcmp((char *)node->name, "livebox")) {
		ErrPrint("Invalid tag: %s\n", (char *)node->name);
		return -EINVAL;
	}

	if (!xmlHasProp(node, (xmlChar *)"appid")) {
		ErrPrint("Missing appid\n");
		return -EINVAL;
	}

	livebox = calloc(1, sizeof(*livebox));
	if (!livebox) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	appid = (char *)xmlGetProp(node, (xmlChar *)"appid");
	if (!appid || validate_appid(pkgname, appid) < 0) {
		ErrPrint("Invalid appid\n");
		xmlFree((xmlChar *)appid);
		free(livebox);
		return -EINVAL;
	}

	livebox->appid = strdup(appid);
	if (!livebox->appid) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree((xmlChar *)appid);
		free(livebox);
		return -EINVAL;
	}

	if (xmlHasProp(node, (xmlChar *)"secured")) {
		tmp = (char *)xmlGetProp(node, (xmlChar *)"secured");
		livebox->secured = !strcasecmp(tmp, "true");
		xmlFree((xmlChar *)tmp);
	}

	if (xmlHasProp(node, (xmlChar *)"auto_launch")) {
		tmp = (char *)xmlGetProp(node, (xmlChar *)"auto_launch");
		livebox->auto_launch = !strcasecmp(tmp, "true");
		xmlFree((xmlChar *)tmp);
	}

	if (xmlHasProp(node, (xmlChar *)"network")) {
		tmp = (char *)xmlGetProp(node, (xmlChar *)"network");
		livebox->network = !strcasecmp(tmp, "true");
		xmlFree((xmlChar *)tmp);
	}

	if (xmlHasProp(node, (xmlChar *)"abi")) {
		tmp = (char *)xmlGetProp(node, (xmlChar *)"abi");
		if (validate_abi(tmp) < 0) {
			xmlFree((xmlChar *)tmp);
			livebox_destroy(livebox);
			return -EINVAL;
		}

		livebox->abi = strdup(tmp);
		xmlFree((xmlChar *)tmp);
	} else {
		livebox->abi = strdup("c");
	}

	if (!livebox->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		livebox_destroy(livebox);
		return -ENOMEM;
	}

	for (node = node->children; node; node = node->next) {
		if (!strcasecmp((char *)node->name, "label")) {
			update_i18n_name(livebox, node);
			continue;
		}

		if (!strcasecmp((char *)node->name, "icon")) {
			update_i18n_icon(livebox, node);
			continue;
		}

		if (!strcasecmp((char *)node->name, "box")) {
			update_box(livebox, node);
			continue;
		}

		if (!strcasecmp((char *)node->name, "pd")) {
			update_pd(livebox, node);
			continue;
		}
	}

	return db_insert_livebox(livebox, pkgname);
}

int PKGMGR_PARSER_PLUGIN_UPGRADE(xmlDocPtr docPtr, const char *appid)
{
	if (!s_info.handle)
		db_init();

	if (!s_info.handle)
		return -EIO;

	return 0;
}

int PKGMGR_PARSER_PLUGIN_UNINSTALL(xmlDocPtr docPtr, const char *pkgname)
{
	xmlNodePtr node;
	char *appid;
	int ret;

	if (!s_info.handle)
		db_init();

	if (!s_info.handle) {
		ErrPrint("Failed to init DB\n");
		return -EIO;
	}

	node = xmlDocGetRootElement(docPtr);
	if (!node) {
		ErrPrint("Invalid document\n");
		return -EINVAL;
	}

	if (strcmp((char *)node->name, "livebox")) {
		ErrPrint("Invalid tag: %s\n", (char *)node->name);
		return -EINVAL;
	}

	if (!xmlHasProp(node, (xmlChar *)"appid")) {
		ErrPrint("Missing appid\n");
		return -EINVAL;
	}

	appid = (char *)xmlGetProp(node, (xmlChar *)"appid");
	if (validate_appid(pkgname, appid) < 0) {
		ErrPrint("Invalid package\n");
		xmlFree((xmlChar *)appid);
		return -EINVAL;
	}

	begin_transaction();
	ret = db_remove_box_size(appid);
	if (ret < 0)
		goto errout;

	ret = db_remove_i18n(appid);
	if (ret < 0)
		goto errout;

	ret = db_remove_client(appid);
	if (ret < 0)
		goto errout;

	ret = db_remove_provider(appid);
	if (ret < 0)
		goto errout;

	ret = db_remove_pkgmap(appid);
	if (ret < 0)
		goto errout;
	commit_transaction();

	xmlFree((xmlChar *)appid);
	return 0;

errout:
	rollback_transaction();
	xmlFree((xmlChar *)appid);
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
