#include <stdio.h>
#include <libgen.h>
#include <ctype.h>
#include <stdlib.h> /* malloc */
#include <errno.h>
#include <string.h> /* strdup */

#include <dlog.h>
#include <Eina.h>

#include "debug.h"
#include "group.h"

int errno;

static struct info {
	Eina_List *cluster_list;
} s_info = {
	.cluster_list = NULL,
};

struct cluster {
	char *name;
	Eina_List *category_list;
};

struct category {
	char *name;
	struct cluster *cluster;
	Eina_List *pkg_list; /* list of instances of the struct inst_info */
};

struct cluster *group_create_cluster(const char *name)
{
	struct cluster *cluster;

	cluster = malloc(sizeof(*cluster));
	if (!cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	cluster->name = strdup(name);
	if (!cluster->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(cluster);
		return NULL;
	}

	cluster->category_list = NULL;

	s_info.cluster_list = eina_list_append(s_info.cluster_list, cluster);
	return cluster;
}

struct cluster *group_find_cluster(const char *name)
{
	Eina_List *l;
	struct cluster *cluster;

	EINA_LIST_FOREACH(s_info.cluster_list, l, cluster) {
		if (!strcasecmp(cluster->name, name))
			return cluster;
	}

	return NULL;
}

struct category *group_create_category(struct cluster *cluster, const char *name)
{
	struct category *category;

	category = malloc(sizeof(*category));
	if (!category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	category->name = strdup(name);
	if (!category->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(category);
		return NULL;
	}

	category->cluster = cluster;
	category->pkg_list = NULL;

	cluster->category_list = eina_list_append(cluster->category_list, category);
	return category;
}

static inline void destroy_cluster(struct cluster *cluster)
{
	free(cluster->name);
	free(cluster);
}

int group_destroy_cluster(struct cluster *cluster)
{
	Eina_List *l;
	Eina_List *n;
	struct cluster *item;

	EINA_LIST_FOREACH_SAFE(s_info.cluster_list, l, n, item) {
		if (item == cluster) {
			s_info.cluster_list = eina_list_remove_list(s_info.cluster_list, l);
			destroy_cluster(cluster);
			return 0;
		}
	}

	return -ENOENT;
}

static inline void destroy_category(struct category *category)
{
	free(category->name);
	free(category);
}

int group_destroy_category(struct category *category)
{
	struct cluster *cluster;
	char *name;

	cluster = category->cluster;
	if (cluster)
		cluster->category_list = eina_list_remove(cluster->category_list, category);

	EINA_LIST_FREE(category->pkg_list, name) {
		free(name);
	}

	destroy_category(category);
	return 0;
}

struct category *group_find_category(struct cluster *cluster, const char *name)
{
	struct category *category;
	Eina_List *l;

	EINA_LIST_FOREACH(cluster->category_list, l, category) {
		if (!strcasecmp(category->name, name))
			return category;
	}

	return NULL;
}

int group_list_category_pkgs(struct category *category, int (*cb)(struct category *category, const char *pkgname, void *data), void *data)
{
	Eina_List *l;
	char *pkgname;

	if (!cb || !category)
		return -EINVAL;

	EINA_LIST_FOREACH(category->pkg_list, l, pkgname) {
		if (cb(category, pkgname, data) == EXIT_FAILURE)
			return -ECANCELED;
	}

	return 0;
}

const char * const group_category_name(struct category *category)
{
	return category->name;
}

const char * const group_cluster_name(struct cluster *cluster)
{
	return cluster->name;
}

const char *group_cluster_name_by_category(struct category *category)
{
	return !category ? NULL : (category->cluster ? category->cluster->name : NULL);
}

int group_add_livebox(const char *group, const char *pkgname)
{
	struct cluster *cluster;
	struct category *category;
	char *name;
	char *ptr;
	int len;
	enum {
		CLUSTER,
		CATEGORY,
	} state;

	state = CLUSTER;

	ptr = (char *)group;
	len = 0;

	/* Skip the first space characters */
	while (*ptr && isspace(*ptr)) ptr++;

	cluster = NULL;
	while (*ptr) {
		if (*ptr == '{') {
			if (len == 0)
				return -EINVAL;

			if (state == CATEGORY)
				return -EINVAL;

			name = malloc(len + 1);
			if (!name)
				return -ENOMEM;

			strncpy(name, ptr - len, len);
			name[len--] = '\0';
			while (isspace(name[len])) {
				name[len] = '\0';
				len--;
			}

			cluster = group_find_cluster(name);
			if (!cluster)
				cluster = group_create_cluster(name);

			free(name);

			if (!cluster) {
				ErrPrint("Failed to get cluster for %s\n", name);
				return -EFAULT;
			}

			state = CATEGORY;
			len = 0;
			while (*ptr && isspace(*ptr++));
			continue;
		}

		if (*ptr == ',' || *ptr == '}') {
			if (state == CATEGORY) {
				if (len == 0)
					return -EINVAL;

				if (!cluster)
					return -EINVAL;

				name = malloc(len + 1);
				if (!name)
					return -ENOMEM;

				strncpy(name, ptr - len, len);
				name[len--] = '\0';
				while (isspace(name[len])) {
					name[len] = '\0';
					len--;
				}

				category = group_find_category(cluster, name);
				if (!category)
					category = group_create_category(cluster, name);
				free(name);

				if (!category)
					return -EFAULT;

				name = strdup(pkgname);
				if (!name) {
					ErrPrint("Heap: %s (%s)\n", strerror(errno), pkgname);
					return -ENOMEM;
				}

				category->pkg_list = eina_list_append(category->pkg_list, name);

				if (*ptr == '}')
					state = CLUSTER;

				len = 0;
				while (*ptr && isspace(*ptr++));
				continue;
			} else {
				len = -1;
				/* Will be ZERO by following increment code */
			}
		}

		len++;
		ptr++;
	}

	if (state != CLUSTER)
		return -EINVAL;

	return 0;
}

int group_del_livebox(const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	Eina_List *s_l;
	Eina_List *s_n;
	Eina_List *p_l;
	Eina_List *p_n;
	struct cluster *cluster;
	struct category *category;
	char *name;

	EINA_LIST_FOREACH_SAFE(s_info.cluster_list, l, n, cluster) {
		EINA_LIST_FOREACH_SAFE(cluster->category_list, s_l, s_n, category) {
			EINA_LIST_FOREACH_SAFE(category->pkg_list, p_l, p_n, name) {
				if (!strcmp(name, pkgname)) {
					category->pkg_list = eina_list_remove_list(category->pkg_list, p_l);
					free(name);
					break;
				}
			}

			if (!category->pkg_list)
				group_destroy_category(category);
		}

		if (!cluster->category_list)
			group_destroy_cluster(cluster);
	}

	return 0;
}

int group_init(void)
{
	return 0;
}

int group_fini(void)
{
	struct cluster *cluster;
	struct category *category;
	char *name;

	EINA_LIST_FREE(s_info.cluster_list, cluster) {
		EINA_LIST_FREE(cluster->category_list, category) {
			EINA_LIST_FREE(category->pkg_list, name) {
				free(name);
			}
			destroy_category(category);
		}
		destroy_cluster(cluster);
	}
	return 0;
}

/* End of a file */
