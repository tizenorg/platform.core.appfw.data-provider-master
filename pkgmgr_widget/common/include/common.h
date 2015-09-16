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
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <string.h>

#include <sqlite3.h>
#include <db-util.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <dlog.h>

#include <widget_service.h>

#include "dlist.h"

#if !defined(FLOG)
#define DbgPrint(format, arg...)	SECURE_LOGD("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg)
#define ErrPrint(format, arg...)	SECURE_LOGE("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg)
#define ErrPrintWithConsole(format, arg...)	do { fprintf(stderr, "[%s/%s:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg); SECURE_LOGE("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg); } while (0)
#endif

#define CUR_VER 6
#define DEFAULT_CATEGORY	"http://tizen.org/category/default"

extern int begin_transaction(void);
extern int commit_transaction(void);

extern int pkglist_get_via_callback(const char *appid, int is_watch_widget, void (*cb)(const char *appid, const char *pkgid, int prime, void *data), void *data);

extern void db_upgrade_db_schema(void);

extern int db_install_widget(xmlNodePtr node, const char *appid);
extern int db_install_watchapp(xmlNodePtr node, const char *appid);
extern int db_init(void);
extern int db_fini(void);

extern int db_check(void);
extern void delete_record_cb(const char *appid, const char *pkgid, int prime, void *data);

/* End of a file */
