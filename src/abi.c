#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <Eina.h>
#include <dlog.h>

#include "util.h"
#include "debug.h"

int errno;

struct item {
	char *abi;
	char *pkgname; /*!< Slave package name */
};

static struct {
	Eina_List *list;
} s_abi = {
	.list = NULL,
};

int abi_add_entry(const char *abi, const char *pkgname)
{
	struct item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Failed to add a new entry for abi[%s - %s]\n", abi, pkgname);
		return -ENOMEM;
	}

	item->abi = strdup(abi);
	if (!item->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(item);
		return -ENOMEM;
	}

	item->pkgname = strdup(pkgname);
	if (!item->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(item->abi);
		free(item);
		return -ENOMEM;
	}

	s_abi.list = eina_list_append(s_abi.list, item);
	return 0;
}

int abi_update_entry(const char *abi, const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;
	char *_pkgname;

	_pkgname = strdup(pkgname);
	if (!_pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	EINA_LIST_FOREACH_SAFE(s_abi.list, l, n, item) {
		if (!strcasecmp(item->abi, abi)) {
			free(item->pkgname);
			item->pkgname = _pkgname;
			return 0;
		}
	}

	free(_pkgname);
	return -ENOENT;
}

int abi_del_entry(const char *abi)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;

	EINA_LIST_FOREACH_SAFE(s_abi.list, l, n, item) {
		if (!strcasecmp(item->abi, abi)) {
			s_abi.list = eina_list_remove(s_abi.list, item);
			free(item->abi);
			free(item->pkgname);
			free(item);
			return 0;
		}
	}

	return -ENOENT;
}

const char *abi_find_slave(const char *abi)
{
	Eina_List *l;
	struct item *item;

	EINA_LIST_FOREACH(s_abi.list, l, item) {
		if (!strcasecmp(item->abi, abi))
			return item->pkgname;
	}

	return NULL;
}

/* End of a file */
