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

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <widget_abi.h>

#include "common.h"

#define WIDGET_TAG "widget-application"
#define EAPI __attribute__((visibility("default")))

static inline int remove_all_widgets(const char *appid)
{
	int cnt;

    ErrPrintWithConsole("%s\n", appid);

    begin_transaction();
    cnt = pkglist_get_via_callback(appid, 0, delete_record_cb, NULL);
    commit_transaction();

    if (cnt > 0) {
		DbgPrint("Package[%s] is not deleted: %d\n", appid, cnt);
    }

    return 0;
}

EAPI int PKGMGR_PARSER_PLUGIN_PRE_INSTALL(const char *appid)
{
	widget_abi_init();
    if (!db_check()) {
		if (db_init() < 0) {
			ErrPrintWithConsole("Failed to init DB\n");
			return -EIO;
		}
    }

    db_upgrade_db_schema();

	return remove_all_widgets(appid);
}

EAPI int PKGMGR_PARSER_PLUGIN_POST_INSTALL(const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);
    db_fini();
	widget_abi_fini();
    return 0;
}

EAPI int PKGMGR_PARSER_PLUGIN_INSTALL(xmlDocPtr docPtr, const char *appid)
{
    xmlNodePtr node;
    int ret;

    ErrPrintWithConsole("[%s]\n", appid);

    if (!db_check()) {
		ErrPrintWithConsole("Failed to init DB\n");
		return -EIO;
    }

    node = xmlDocGetRootElement(docPtr);
    if (!node) {
		ErrPrintWithConsole("Invalid document\n");
		return -EINVAL;
    }

    for (node = node->children; node; node = node->next) {
		DbgPrint("node->name: %s\n", node->name);
		if (!xmlStrcasecmp(node->name, (const xmlChar *)WIDGET_TAG)) {
			ret = db_install_widget(node, appid);
			if (ret < 0) {
				DbgPrint("Returns: %d\n", ret);
			}
		}
    }

    return 0;
}

EAPI int PKGMGR_PARSER_PLUGIN_PRE_UPGRADE(const char *appid)
{
	widget_abi_init();
    if (!db_check()) {
		if (db_init() < 0) {
			ErrPrintWithConsole("Failed to init DB\n");
			return -EIO;
		}
    }

    db_upgrade_db_schema();

	return remove_all_widgets(appid);
}

EAPI int PKGMGR_PARSER_PLUGIN_POST_UPGRADE(const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);
    db_fini();
	widget_abi_fini();
    return 0;
}

EAPI int PKGMGR_PARSER_PLUGIN_UPGRADE(xmlDocPtr docPtr, const char *appid)
{
    xmlNodePtr node;
    int ret;

    ErrPrintWithConsole("[%s]\n", appid);

    if (!db_check()) {
		ErrPrint("Failed to init DB\n");
		return -EIO;
    }

    node = xmlDocGetRootElement(docPtr);
    if (!node) {
		ErrPrint("Invalid document\n");
		return -EINVAL;
    }

    for (node = node->children; node; node = node->next) {
		if (!xmlStrcasecmp(node->name, (const xmlChar *)WIDGET_TAG)) {
			ret = db_install_widget(node, appid);
			if (ret < 0) {
				DbgPrint("Returns: %d\n", ret);
			}
		}
    }

    return 0;
}

EAPI int PKGMGR_PARSER_PLUGIN_PRE_UNINSTALL(const char *appid)
{
	widget_abi_init();
    ErrPrintWithConsole("[%s]\n", appid);

    if (!db_check()) {
		if (db_init() < 0) {
			ErrPrint("Failed to init DB\n");
			return -EIO;
		}
    }

    db_upgrade_db_schema();

    return 0;
}

EAPI int PKGMGR_PARSER_PLUGIN_POST_UNINSTALL(const char *appid)
{
	int ret;

	ret = remove_all_widgets(appid);

    db_fini();
	widget_abi_fini();
    return ret;
}

EAPI int PKGMGR_PARSER_PLUGIN_UNINSTALL(xmlDocPtr docPtr, const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);
    if (!db_check()) {
		return -EIO;
    }

    /* Doesn't need to do anything from here, we already dealt it with this */

    return 0;
}

/* End of a file */
