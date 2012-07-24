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
 * DB Table schema
 *
 * provider
 * +-------+-----------+-----+---------+-------+-------+
 * | appid |  network  | abi | secured |  src  | group |
 * +-------+-----------+-----+---------+-------+-------+
 * |   -   |    -      |  -  |    -    |   -   |   -   |
 * +-------+-----------+-----+---------+-------+-------+
 *
 * client
 * +-------+------+---------+-------------+---------------+
 * | appid | Icon |  Name   | auto_launch | box_size_list |
 * +-------+------+---------+-------------+---------------+
 * |   -   |   -  |    -    |      -      |        -      |
 * +-------+------+---------+-------------+---------------+
 *
 * i18n
 * +-------+------+------+
 * |   fk  | lang | name |
 * +-------+------+------+
 * | appid |   -  |   -  |
 * +-------+------+------+
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
	.dbfile = "/opt/dbspace/.shortcut_service.db",
	.handle = NULL,
};

static inline int begin_transaction(void)
{
	sqlite3_stmt *stmt;
	int ret;

	ret = sqlite3_prepare_v2(
		s_info.handle, "BEGIN TRANSACTION", -1, &stmt, NULL);

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

	ret = sqlite3_prepare_v2(
			s_info.handle, "ROLLBACK TRANSACTION", -1, &stmt, NULL);
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

	ret = sqlite3_prepare_v2(
			s_info.handle, "COMMIT TRANSACTION", -1, &stmt, NULL);
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

static inline void db_create_table(void)
{
	char *err;
	static const char *ddl =
		"CREATE TABLE shortcut_service ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"appid TEXT, "
		"icon TEXT, "
		"name TEXT, "
		"extra_key TEXT, "
		"extra_data TEXT)";

	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");

	ddl = "CREATE TABLE shortcut_name (id INTEGER, lang TEXT, name TEXT)";
	if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
		ErrPrint("Failed to execute the DDL (%s)\n", err);
		return;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		ErrPrint("No changes to DB\n");
}

static inline int db_remove_record(const char *appid, const char *key, const char *data)
{
	static const char *dml = "DELETE FROM shortcut_service WHERE appid = ? AND extra_key = ? AND extra_data = ?";
	sqlite3_stmt *stmt;
	int ret;

	if (!appid || !key || !data) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to prepare the initial DML\n");
		return -EIO;
	}

	ret = -EIO;
	if (sqlite3_bind_text(stmt, 1, appid, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a appid(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 2, key, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a key(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 3, data, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a data(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		ret = -EIO;
		ErrPrint("Failed to execute the DML for %s - %s(%s)\n", appid, key, data);
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_remove_name(int id)
{
	static const char *dml = "DELETE FROM shortcut_name WHERE id = ?";
	sqlite3_stmt *stmt;
	int ret;

	if (id < 0) {
		ErrPrint("Inavlid id\n");
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to prepare the initial DML\n");
		return -EIO;
	}

	if (sqlite3_bind_int(stmt, 1, id) != SQLITE_OK) {
		ErrPrint("Failed to bind id(%d)\n", id);
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		ret = -EIO;
		ErrPrint("Failed to execute the DML for %d\n", id);
		goto out;
	}

	if (sqlite3_changes(s_info.handle) == 0)
		DbgPrint("No changes\n");

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_insert_record(const char *appid, const char *icon, const char *name, const char *key, const char *data)
{
	static const char *dml = "INSERT INTO shortcut_service (appid, icon, name, key, data) VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt;
	int ret;

	if (!appid) {
		ErrPrint("Failed to get appid\n");
		return -EINVAL;
	}

	if (!name) {
		ErrPrint("Failed to get name\n");
		return -EINVAL;
	}

	if (!key) {
		ErrPrint("Failed to get key\n");
		return -EINVAL;
	}

	if (!data) {
		ErrPrint("Faield to get key\n");
		return -EINVAL;
	}

	icon = icon ? icon : "";

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to prepare the initial DML\n");
		return -EIO;
	}

	ret = -EIO;
	if (sqlite3_bind_text(stmt, 1, appid, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a appid(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 2, icon, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a icon(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 3, name, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a name(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 4, key, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a service(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 5, data, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a service(%s)\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		ErrPrint("Failed to execute the DML for %s - %s\n", appid, name);
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_insert_name(int id, const char *lang, const char *name)
{
	static const char *dml = "INSERT INTO shortcut_name (id, lang, name) VALUES (?, ?, ?)";
	sqlite3_stmt *stmt;
	int ret;

	if (id < 0 || !lang || !name) {
		ErrPrint("Invalid parameters\n");
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to prepare the initial DML\n");
		return -EIO;
	}

	if (sqlite3_bind_int(stmt, 1, id) != SQLITE_OK) {
		ErrPrint("Failed to bind a id(%s)\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	if (sqlite3_bind_text(stmt, 2, lang, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a id(%s)\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	if (sqlite3_bind_text(stmt, 3, name, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a id(%s)\n", sqlite3_errmsg(s_info.handle));
		ret = -EIO;
		goto out;
	}

	ret = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		ErrPrint("Failed to execute the DML for %d %s %s\n", id, lang, name);
		ret = -EIO;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int db_get_id(const char *appid, const char *key, const char *data)
{
	static const char *dml = "SELECT id FROM shortcut_service WHERE appid = ? AND key = ? AND data = ?";
	sqlite3_stmt *stmt;
	int ret;

	if (!appid || !key || !data) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to prepare the initial DML\n");
		return -EIO;
	}

	ret = -EIO;
	if (sqlite3_bind_text(stmt, 1, appid, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a appid(%s) - %s\n", appid, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 2, key, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a key(%s) - %s\n", key, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_bind_text(stmt, 3, data, -1, NULL) != SQLITE_OK) {
		ErrPrint("Failed to bind a data(%s) - %s\n", data, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("Failed to execute the DML for %s - %s, %s\n", appid, key, data);
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

int PKGMGR_PARSER_PLUGIN_UPGRADE(xmlDocPtr docPtr, const char *appid)
{
	xmlNodePtr root;

	root = xmlDocGetRootElement(docPtr);
	if (!root) {
		ErrPrint("Invalid node ptr\n");
		return -EINVAL;
	}

	if (!s_info.handle) {
		if (db_init() < 0)
			return -EIO;
	}

	if (strcmp((char *)root->name, "shortcut-list")) {
		ErrPrint("Invalid XML root\n");
		return -EINVAL;
	}

	return 0;
}

int PKGMGR_PARSER_PLUGIN_UNINSTALL(xmlDocPtr docPtr, const char *_appid)
{
	xmlNodePtr node = NULL;
	xmlChar *key;
	xmlChar *data;
	xmlChar *appid;
	xmlNodePtr root;
	int id;

	root = xmlDocGetRootElement(docPtr);
	if (!root) {
		ErrPrint("Invalid node ptr\n");
		return -EINVAL;
	}

	if (!s_info.handle) {
		if (db_init() < 0)
			return -EIO;
	}

	if (strcmp((char *)root->name, "shortcut-list")) {
		ErrPrint("Invalid XML root\n");
		return -EINVAL;
	}

	DbgPrint("AppID: %s\n", _appid);
	root = root->children;
	while (root) {
		for (node = root; node; node = node->next) {
			if (node->type == XML_ELEMENT_NODE)
				DbgPrint("Element %s\n", node->name);

			if (strcmp((char *)node->name, "shortcut"))
				continue;

			if (!xmlHasProp(node, (xmlChar *)"extra_data")
				|| !xmlHasProp(node, (xmlChar *)"extra_key")
				|| !xmlHasProp(node, (xmlChar *)"appid"))
			{
				DbgPrint("Invalid element %s\n", node->name);
				continue;
			}

			appid = xmlGetProp(node, (xmlChar *)"appid");
			key = xmlGetProp(node, (xmlChar *)"extra_key");
			data = xmlGetProp(node, (xmlChar *)"extra_data");

			DbgPrint("appid: %s\n", appid);
			DbgPrint("key: %s\n", key);
			DbgPrint("data: %s\n", data);

			id = db_get_id((char *)appid, (char *)key, (char *)data);
			if (id < 0) {
				ErrPrint("No records found\n");
				xmlFree(appid);
				xmlFree(key);
				xmlFree(data);
				continue;
			}

			begin_transaction();
			if (db_remove_record((char *)appid, (char *)key, (char *)data) < 0) {
				ErrPrint("Failed to remove a record\n");
				rollback_transaction();
				xmlFree(appid);
				xmlFree(key);
				xmlFree(data);
				continue;
			}

			if (db_remove_name(id) < 0) {
				ErrPrint("Failed to remove name records\n");
				rollback_transaction();
				xmlFree(appid);
				xmlFree(key);
				xmlFree(data);
				continue;
			}
			commit_transaction();
			xmlFree(appid);
			xmlFree(key);
			xmlFree(data);

			/*!
			 * \note
			 * if (node->children)
			 * DbgPrint("Skip this node's children\n");
			 */
		}
		root = root->next;
	}

	return 0;
}

int PKGMGR_PARSER_PLUGIN_INSTALL(xmlDocPtr docPtr, const char *appid)
{
	xmlNodePtr node = NULL;
	xmlNodePtr child = NULL;
	xmlChar *key;
	xmlChar *data;
	xmlChar *name;
	xmlChar *icon;
	xmlNodePtr root;
	struct i18n_name {
		xmlChar *name;
		xmlChar *lang;
	} *i18n;
	struct dlist *i18n_list = NULL;
	struct dlist *l;
	struct dlist *n;
	int id;

	root = xmlDocGetRootElement(docPtr);
	if (!root) {
		ErrPrint("Invalid node ptr\n");
		return -EINVAL;
	}

	if (!s_info.handle) {
		if (db_init() < 0)
			return -EIO;
	}

	if (strcmp((char *)root->name, "shortcut-list")) {
		ErrPrint("Invalid XML root\n");
		return -EINVAL;
	}

	DbgPrint("AppID: %s\n", appid);

	root = root->children;
	while (root) {
		for (node = root; node; node = node->next) {
			if (node->type == XML_ELEMENT_NODE)
				DbgPrint("Element %s\n", node->name);

			if (strcmp((char *)node->name, "shortcut"))
				continue;

			if (!xmlHasProp(node, (xmlChar *)"extra_key") || !xmlHasProp(node, (xmlChar *)"extra_data")) {
				DbgPrint("Invalid element %s\n", node->name);
				continue;
			}

			key = xmlGetProp(node, (xmlChar *)"extra_key");
			data = xmlGetProp(node, (xmlChar *)"extra_data");

			icon = NULL;
			name = NULL;
			for (child = node->children; child; child = child->next) {
				if (!strcmp((char *)child->name, "icon")) {
					if (icon) {
						DbgPrint("Icon is duplicated\n");
						continue;
					}

					icon = xmlNodeGetContent(child);
					continue;
				}

				if (!strcmp((char *)child->name, "label")) {
					if (!xmlHasProp(child, (xmlChar *)"xml:lang") && name) {
						DbgPrint("Default name is duplicated\n");
						continue;
					}

					i18n = malloc(sizeof(*i18n));
					if (!i18n) {
						ErrPrint("Heap: %s\n", strerror(errno));
						break;
					}

					i18n->lang = xmlGetProp(child, (xmlChar *)"xml:lang");
					i18n->name = xmlNodeGetContent(child);
					i18n_list = dlist_append(i18n_list, i18n);
					continue;
				}

			}

			DbgPrint("appid: %s\n", appid);
			DbgPrint("key: %s\n", key);
			DbgPrint("data: %s\n", data);
			DbgPrint("icon: %s\n", icon);
			DbgPrint("Default name: %s\n", name);

			begin_transaction();
			if (db_insert_record(appid, (char *)icon, (char *)name, (char *)key, (char *)data) < 0) {
				ErrPrint("Failed to insert a new record\n");
				rollback_transaction();
				xmlFree((xmlChar *)appid);
				xmlFree(key);
				xmlFree(data);
				xmlFree(icon);
				xmlFree(name);

				dlist_foreach_safe(i18n_list, l, n, i18n) {
					i18n_list = dlist_remove(i18n_list, l);
					xmlFree(i18n->lang);
					xmlFree(i18n->name);
					free(i18n);
				}
			} else {
				id = db_get_id(appid, (char *)key, (char *)data);
				if (id < 0) {
					ErrPrint("Failed to insert a new record\n");
					rollback_transaction();
					xmlFree((xmlChar *)appid);
					xmlFree(key);
					xmlFree(data);
					xmlFree(icon);
					xmlFree(name);

					dlist_foreach_safe(i18n_list, l, n, i18n) {
						i18n_list = dlist_remove(i18n_list, l);
						xmlFree(i18n->lang);
						xmlFree(i18n->name);
						free(i18n);
					}
				} else {
					dlist_foreach_safe(i18n_list, l, n, i18n) {
						i18n_list = dlist_remove(i18n_list, l);
						if (db_insert_name(id, (char *)i18n->lang, (char *)i18n->name) < 0)
							ErrPrint("Failed to add i18n name: %s(%s)\n", i18n->name, i18n->lang);
						xmlFree(i18n->lang);
						xmlFree(i18n->name);
						free(i18n);
					}
					commit_transaction();
				}
			}
		}
		root = root->next;
	}

	return 0;
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
