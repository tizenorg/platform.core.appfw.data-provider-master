#include <stdio.h>
#include <stdbool.h>
#include <libgen.h>
#include <unistd.h>

#include <Ecore.h>
#include <gio/gio.h>

#include <dlog.h>
#include <vconf.h>
//#include <context_subscribe.h>

#include "debug.h"
#include "slave_manager.h"
#include "client_manager.h"
#include "pkg_manager.h"
#include "group.h"
#include "conf.h"
#include "util.h"
#include "rpc_to_slave.h"

static struct info {
	int updated;
	unsigned long pending_mask;
} s_info = {
	.updated = 0,
	.pending_mask = 0,
};

static int update_pkg_cb(struct category *category, const char *pkgname, void *data)
{
	const char *c_name;
	const char *s_name;

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid\n");
		return EXIT_FAILURE;
	}

	rpc_send_update_request(pkgname, c_name, s_name);
	rpc_send_create_request(NULL, pkgname, "default", c_name, s_name, util_get_timestamp(), DEFAULT_PERIOD);
	return EXIT_SUCCESS;
}

static inline void update_location(void)
{
	struct cluster *cluster;
	struct category *category;

	cluster = group_find_cluster("location");
	if (cluster) {
		category = group_find_category(cluster, "location_now");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "location_apps");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "location_appointment");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "location_weather");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}

	cluster = group_find_cluster("apps");
	if (cluster) {
		category = group_find_category(cluster, "apps_location");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}

	cluster = group_find_cluster("people");
	if (cluster) {
		category = group_find_category(cluster, "people_at");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}

	cluster = group_find_cluster("photos_videos");
	if (cluster) {
		category = group_find_category(cluster, "media_location");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}
}

static inline void update_contacts(void)
{
	struct cluster *cluster;
	struct category *category;

	cluster = group_find_cluster("people");
	if (cluster) {
		category = group_find_category(cluster, "people_frequently");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "people_rarely");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "people_during");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "people_at");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}
}

static inline void update_apps(void)
{
	struct cluster *cluster;
	struct category *category;

	cluster = group_find_cluster("apps");
	if (cluster) {
		category = group_find_category(cluster, "apps_frequently");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);

		category = group_find_category(cluster, "apps_location");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}

	cluster = group_find_cluster("location");
	if (cluster) {
		category = group_find_category(cluster, "location_apps");
		if (category)
			group_list_category_pkgs(category, update_pkg_cb, NULL);
	}
}

static inline void update_music(void)
{
	struct cluster *cluster;
	struct category *category;

	cluster = group_find_cluster("music");
	if (!cluster)
		return;

	category = group_find_category(cluster, "music_top_album");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);

	category = group_find_category(cluster, "music_new_album");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);

	category = group_find_category(cluster, "music_recently");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);

	category = group_find_category(cluster, "music_top_track");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);
}

static inline void update_photo(void)
{
	struct cluster *cluster;
	struct category *category;

	cluster = group_find_cluster("photos_videos");
	if (!cluster)
		return;

	category = group_find_category(cluster, "media_latest");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);

	category = group_find_category(cluster, "media_location");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);

	category = group_find_category(cluster, "facebook_media");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);
}

void ctx_update(void)
{
/*
	if (s_info.pending_mask & CONTEXT_NOTI_LOCATION)
		update_location();

	if (s_info.pending_mask & CONTEXT_NOTI_CONTACTS)
		update_contacts();

	if (s_info.pending_mask & CONTEXT_NOTI_APPS)
		update_apps();

	if (s_info.pending_mask & CONTEXT_NOTI_MUSIC)
		update_music();

	if (s_info.pending_mask & CONTEXT_NOTI_PHOTOS)
		update_photo();

*/
	s_info.pending_mask = 0;
	return;
}
/*
static bool ctx_changed_cb(context_type_e type, void *user_data)
{
	if (client_is_all_paused()) {
		s_info.pending_mask |= type;
		return false;
	}

	switch (type) {
	case CONTEXT_NOTI_LOCATION:
		update_location();
		break;
	case CONTEXT_NOTI_CONTACTS:
		update_contacts();
		break;
	case CONTEXT_NOTI_APPS:
		update_apps();
		break;
	case CONTEXT_NOTI_MUSIC:
		update_music();
		break;
	case CONTEXT_NOTI_PHOTOS:
		update_photo();
		break;
	default:
		break;
	}

	return false;
}
*/

int ctx_client_init(void)
{
/*
	context_set_context_changed_cb(ctx_changed_cb,
		CONTEXT_NOTI_LOCATION | CONTEXT_NOTI_CONTACTS | CONTEXT_NOTI_APPS |
		CONTEXT_NOTI_MUSIC | CONTEXT_NOTI_PHOTOS, NULL);
*/
	return 0;
}

int ctx_client_fini(void)
{
	return 0;
}

/* End of a file */
