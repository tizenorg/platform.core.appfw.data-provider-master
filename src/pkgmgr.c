#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include <dlog.h>
#include <package-manager.h>

#include <Ecore.h>
#include "util.h"
#include "debug.h"
#include "pkgmgr.h"

struct item {
	char *pkgname;
	char *icon;

	enum pkgmgr_event_type type;
	enum pkgmgr_status status;
};

static struct {
	pkgmgr_client *listen_pc;
	Eina_List *item_list;

	Eina_List *install_event;
	Eina_List *uninstall_event;
	Eina_List *update_event;
	Eina_List *download_event;
	Eina_List *recover_event;
} s_info = {
	.listen_pc = NULL,
	.item_list = NULL,

	.install_event = NULL,
	.uninstall_event = NULL,
	.update_event = NULL,
	.download_event = NULL,
	.recover_event = NULL,
};

struct event_item {
	int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data);
	void *data;
};

static inline void invoke_install_event_handler(const char *pkgname, enum pkgmgr_status status, double value)
{
	Eina_List *l;
	struct event_item *item;

	EINA_LIST_FOREACH(s_info.install_event, l, item) {
		if (item->cb)
			item->cb(pkgname, status, value, item->data);
	}
}

static inline void invoke_uninstall_event_handler(const char *pkgname, enum pkgmgr_status status, double value)
{
	Eina_List *l;
	struct event_item *item;

	EINA_LIST_FOREACH(s_info.uninstall_event, l, item) {
		if (item->cb)
			item->cb(pkgname, status, value, item->data);
	}
}

static inline void invoke_update_event_handler(const char *pkgname, enum pkgmgr_status status, double value)
{
	Eina_List *l;
	struct event_item *item;

	EINA_LIST_FOREACH(s_info.update_event, l, item) {
		if (item->cb)
			item->cb(pkgname, status, value, item->data);
	}
}

static inline void invoke_download_event_handler(const char *pkgname, enum pkgmgr_status status, double value)
{
	Eina_List *l;
	struct event_item *item;

	EINA_LIST_FOREACH(s_info.download_event, l, item) {
		if (item->cb)
			item->cb(pkgname, status, value, item->data);
	}
}

static inline void invoke_recover_event_handler(const char *pkgname, enum pkgmgr_status status, double value)
{
	Eina_List *l;
	struct event_item *item;

	EINA_LIST_FOREACH(s_info.recover_event, l, item) {
		if (item->cb)
			item->cb(pkgname, status, value, item->data);
	}
}

static inline void invoke_callback(const char *pkgname, struct item *item, double value)
{
	switch (item->type) {
	case PKGMGR_EVENT_DOWNLOAD:
		invoke_download_event_handler(pkgname, item->status, value);
		break;
	case PKGMGR_EVENT_UNINSTALL:
		invoke_uninstall_event_handler(pkgname, item->status, value);
		break;
	case PKGMGR_EVENT_INSTALL:
		invoke_install_event_handler(pkgname, item->status, value);
		break;
	case PKGMGR_EVENT_UPDATE:
		invoke_update_event_handler(pkgname, item->status, value);
		break;
	case PKGMGR_EVENT_RECOVER:
		invoke_recover_event_handler(pkgname, item->status, value);
		break;
	default:
		break;
	}
}

static inline int is_valid_status(struct item *item, const char *status)
{
	const char *expected_status;

	switch (item->type) {
	case PKGMGR_EVENT_DOWNLOAD:
		expected_status = "download";
		break;
	case PKGMGR_EVENT_UNINSTALL:
		expected_status = "uninstall";
		break;
	case PKGMGR_EVENT_INSTALL:
		expected_status = "install";
		break;
	case PKGMGR_EVENT_UPDATE:
		expected_status = "update";
		break;
	case PKGMGR_EVENT_RECOVER:
		expected_status = "recover";
		break;
	default:
		return 0;
	}

	return !strcasecmp(status, expected_status);
}

static struct item *find_item(const char *pkgname)
{
	Eina_List *l;
	struct item *item;

	if (!pkgname) {
		ErrPrint("Package name is not valid\n");
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.item_list, l, item) {
		if (strcmp(item->pkgname, pkgname))
			continue;

		return item;
	}

	DbgPrint("Package %s is not found\n", pkgname);
	return NULL;
}

static int start_cb(const char *pkgname, const char *val, void *data)
{
	struct item *item;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	item->pkgname = strdup(pkgname);
	if (!item->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return -ENOMEM;
	}

	item->status = PKGMGR_STATUS_START;
	s_info.item_list = eina_list_append(s_info.item_list, item);

	if (!strcasecmp(val, "download")) {
		item->type = PKGMGR_EVENT_DOWNLOAD;
	} else if (!strcasecmp(val, "uninstall")) {
		item->type = PKGMGR_EVENT_UNINSTALL;
	} else if (!strcasecmp(val, "install")) {
		item->type = PKGMGR_EVENT_INSTALL;
	} else if (!strcasecmp(val, "update")) {
		item->type = PKGMGR_EVENT_UPDATE;
	} else if (!strcasecmp(val, "recover")) {
		item->type = PKGMGR_EVENT_RECOVER;
	} else {
		DbgFree(item->pkgname);
		DbgFree(item);
		ErrPrint("Invalid val: %s\n", val);
		return -EINVAL;
	}

	invoke_callback(pkgname, item, 0.0f);
	return 0;
}

static int icon_path_cb(const char *pkgname, const char *val, void *data)
{
	struct item *item;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item)
		return -ENOENT;

	if (item->icon)
		DbgFree(item->icon);

	item->icon = strdup(val);
	if (!item->icon) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	return 0;
}

static int command_cb(const char *pkgname, const char *val, void *data)
{
	struct item *item;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item)
		return -ENOENT;

	if (!is_valid_status(item, val)) {
		DbgPrint("Invalid status: %d, %s\n", item->type, val);
		return -EINVAL;
	}

	item->status = PKGMGR_STATUS_COMMAND;
	invoke_callback(pkgname, item, 0.0f);
	return 0;
}

static int error_cb(const char *pkgname, const char *val, void *data)
{
	/* val = error */
	struct item *item;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item)
		return -ENOENT;

	item->status = PKGMGR_STATUS_ERROR;
	invoke_callback(pkgname, item, 0.0f);
	return 0;
}

static int change_pkgname_cb(const char *pkgname, const char *val, void *data)
{
	struct item *item;
	char *new_pkgname;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item)
		return -ENOENT;

	new_pkgname = strdup(val);
	if (!new_pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	DbgFree(item->pkgname);
	item->pkgname = new_pkgname;
	return 0;
}

static int download_cb(const char *pkgname, const char *val, void *data)
{
	/* val = integer */
	struct item *item;
	double value;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item) {
		DbgPrint("ITEM is not started from the start_cb\n");
		start_cb(pkgname, "download", data);

		item = find_item(pkgname);
		if (!item) {
			ErrPrint("Package %s has no valid state\n", pkgname);
			return -EINVAL;
		}
	}

	if (item->type != PKGMGR_EVENT_DOWNLOAD) {
		DbgPrint("TYPE is not \"install\" : %d\n", item->type);
		item->type = PKGMGR_EVENT_DOWNLOAD;
	}

	switch (item->status) {
	case PKGMGR_STATUS_START:
	case PKGMGR_STATUS_COMMAND:
		item->status = PKGMGR_STATUS_PROCESSING;
	case PKGMGR_STATUS_PROCESSING:
		break;
	default:
		ErrPrint("Invalid state [%s, %s]\n", pkgname, val);
		return -EINVAL;
	}

	if (val) {
		if (sscanf(val, "%lf", &value) != 1)
			value = (double)-EINVAL;
	} else {
		value = (double)-EINVAL;
	}

	invoke_download_event_handler(pkgname, item->status, value);
	return 0;
}

static int install_cb(const char *pkgname, const char *val, void *data)
{
	/* val = integer */
	struct item *item;
	double value;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item) {
		DbgPrint("ITEM is not started from the start_cb\n");
		start_cb(pkgname, "install", data);

		item = find_item(pkgname);
		if (!item) {
			ErrPrint("Package %s has no valid state\n", pkgname);
			return -EINVAL;
		}
	}

	if (item->type != PKGMGR_EVENT_INSTALL) {
		DbgPrint("TYPE is not \"install\" : %d\n", item->type);
		item->type = PKGMGR_EVENT_INSTALL;
	}

	switch (item->status) {
	case PKGMGR_STATUS_START:
	case PKGMGR_STATUS_COMMAND:
		item->status = PKGMGR_STATUS_PROCESSING;
	case PKGMGR_STATUS_PROCESSING:
		break;
	default:
		ErrPrint("Invalid state [%s, %s]\n", pkgname, val);
		return -EINVAL;
	}

	if (val) {
		if (sscanf(val, "%lf", &value) != 1)
			value = (double)-EINVAL;
	} else {
		value = (double)-EINVAL;
	}

	invoke_install_event_handler(pkgname, item->status, value);
	return 0;
}

static int end_cb(const char *pkgname, const char *val, void *data)
{
	struct item *item;

	DbgPrint("[%s] %s\n", pkgname, val);

	item = find_item(pkgname);
	if (!item)
		return -ENOENT;

	item->status = !strcasecmp(val, "ok") ? PKGMGR_STATUS_END : PKGMGR_STATUS_ERROR;

	invoke_callback(pkgname, item, 0.0f);

	s_info.item_list = eina_list_remove(s_info.item_list, item);
	DbgFree(item->icon);
	DbgFree(item->pkgname);
	DbgFree(item);
	return 0;
}

struct pkgmgr_handler {
	const char *key;
	int (*func)(const char *package, const char *val, void *data);
} handler[] = {
	{ "install_percent", install_cb },
	{ "start", start_cb },
	{ "end", end_cb },
	{ "download_percent", download_cb },
	{ "change_pkg_name", change_pkgname_cb },
	{ "icon_path", icon_path_cb },
	{ "command", command_cb },
	{ "error", error_cb },
	{ NULL, NULL },
};

static int pkgmgr_cb(int req_id, const char *type, const char *pkgname, const char *key, const char *val, const void *pmsg, void *data)
{
	register int i;
	int ret;

	for (i = 0; handler[i].key; i++) {
		if (strcasecmp(key, handler[i].key))
			continue;

		ret = handler[i].func(pkgname, val, data);
		DbgPrint("REQ[%d] pkgname[%s], type[%s], key[%s], val[%s], ret = %d\n",
						req_id, pkgname, type, key, val, ret);
	}

	return 0;
}

int pkgmgr_init(void)
{
	if (s_info.listen_pc)
		return -EALREADY;

	s_info.listen_pc = pkgmgr_client_new(PC_LISTENING);
	if (!s_info.listen_pc)
		return -EFAULT;

	if (pkgmgr_client_listen_status(s_info.listen_pc, pkgmgr_cb, NULL) != PKGMGR_R_OK)
		return -EFAULT;

	return 0;
}

int pkgmgr_fini(void)
{
	struct event_item *item;
	struct item *ctx;

	if (!s_info.listen_pc)
		return -EINVAL;

	if (pkgmgr_client_free(s_info.listen_pc) != PKGMGR_R_OK)
		return -EFAULT;

	s_info.listen_pc = NULL;

	EINA_LIST_FREE(s_info.download_event, item) {
		DbgFree(item);
	}

	EINA_LIST_FREE(s_info.uninstall_event, item) {
		DbgFree(item);
	}

	EINA_LIST_FREE(s_info.install_event, item) {
		DbgFree(item);
	}

	EINA_LIST_FREE(s_info.update_event, item) {
		DbgFree(item);
	}

	EINA_LIST_FREE(s_info.recover_event, item) {
		DbgFree(item);
	}

	EINA_LIST_FREE(s_info.item_list, ctx) {
		DbgFree(ctx->pkgname);
		DbgFree(ctx->icon);
		DbgFree(ctx);
	}

	return 0;
}

int pkgmgr_add_event_callback(enum pkgmgr_event_type type, int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data), void *data)
{
	struct event_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	item->cb = cb;
	item->data = data;

	switch (type) {
	case PKGMGR_EVENT_DOWNLOAD:
		s_info.download_event = eina_list_prepend(s_info.download_event, item);
		break;
	case PKGMGR_EVENT_UNINSTALL:
		s_info.uninstall_event = eina_list_prepend(s_info.uninstall_event, item);
		break;
	case PKGMGR_EVENT_INSTALL:
		s_info.install_event = eina_list_prepend(s_info.install_event, item);
		break;
	case PKGMGR_EVENT_UPDATE:
		s_info.update_event = eina_list_prepend(s_info.update_event, item);
		break;
	case PKGMGR_EVENT_RECOVER:
		s_info.recover_event = eina_list_prepend(s_info.recover_event, item);
		break;
	default:
		DbgFree(item);
		return -EINVAL;
	}

	return 0;
}

void *pkgmgr_del_event_callback(enum pkgmgr_event_type type, int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data), void *data)
{
	struct event_item *item;
	Eina_List *l;
	void *cbdata = NULL;

	switch (type) {
	case PKGMGR_EVENT_DOWNLOAD:
		EINA_LIST_FOREACH(s_info.download_event, l, item) {
			if (item->cb == cb && item->data == data) {
				s_info.download_event = eina_list_remove(s_info.download_event, item);
				cbdata = item->data;
				DbgFree(item);
				break;
			}
		}
		break;
	case PKGMGR_EVENT_UNINSTALL:
		EINA_LIST_FOREACH(s_info.uninstall_event, l, item) {
			if (item->cb == cb && item->data == data) {
				s_info.uninstall_event = eina_list_remove(s_info.uninstall_event, item);
				cbdata = item->data;
				DbgFree(item);
				break;
			}
		}
		break;
	case PKGMGR_EVENT_INSTALL:
		EINA_LIST_FOREACH(s_info.install_event, l, item) {
			if (item->cb == cb && item->data == data) {
				s_info.install_event = eina_list_remove(s_info.install_event, item);
				cbdata = item->data;
				DbgFree(item);
				break;
			}
		}
		break;
	case PKGMGR_EVENT_UPDATE:
		EINA_LIST_FOREACH(s_info.update_event, l, item) {
			if (item->cb == cb && item->data == data) {
				s_info.update_event = eina_list_remove(s_info.update_event, item);
				cbdata = item->data;
				DbgFree(item);
				break;
			}
		}
		break;
	case PKGMGR_EVENT_RECOVER:
		EINA_LIST_FOREACH(s_info.recover_event, l, item) {
			if (item->cb == cb && item->data == data) {
				s_info.recover_event = eina_list_remove(s_info.recover_event, item);
				cbdata = item->data;
				DbgFree(item);
				break;
			}
		}
		break;
	default:
		ErrPrint("Invalid type\n");
		break;
	}

	return cbdata;
}

/* End of a file */
