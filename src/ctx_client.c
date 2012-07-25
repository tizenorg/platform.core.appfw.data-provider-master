#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include <Ecore.h>
#include <gio/gio.h>

#include <vconf.h>
#include <dlog.h>
#include <context_subscribe.h>
#include <packet.h>

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "group.h"
#include "conf.h"
#include "util.h"
#include "rpc_to_slave.h"
#include "setting.h"

#define SYS_CLUSTER_KEY "file/private/com.samsung.cluster-home/system_cluster"

static struct info {
	int updated;
	int enabled;
	unsigned long pending_mask;
} s_info = {
	.updated = 0,
	.enabled = 0,
	.pending_mask = 0x0,
};

static int update_pkg_cb(struct category *category, const char *pkgname, void *data)
{
	const char *c_name;
	const char *s_name;
	double timestamp;
	struct inst_info *inst;

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid\n");
		return EXIT_FAILURE;
	}

	slave_rpc_request_update(pkgname, c_name, s_name);

	/* Just try to create a new package */
	timestamp = util_timestamp();
	inst = instance_create(NULL, timestamp, pkgname, DEFAULT_CONTENT, c_name, s_name, DEFAULT_PERIOD);
	return EXIT_SUCCESS;
}

static inline void update_location(void)
{
	struct cluster *cluster;
	struct category *category;

	DbgPrint("Processing events: Location\n");

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

	DbgPrint("Processing events: Contacts\n");

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

	DbgPrint("Processing events: Apps\n");

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

	DbgPrint("Processing events: Music\n");

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

	DbgPrint("Processing events: Photo\n");

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

static inline void update_keyword(void)
{
	struct cluster *cluster;
	struct category *category;

	DbgPrint("Processing events: Keyword\n");

	cluster = group_find_cluster("news");
	if (!cluster)
		return;

	category = group_find_category(cluster, "news_from");
	if (category)
		group_list_category_pkgs(category, update_pkg_cb, NULL);
}

void ctx_update(unsigned long mask)
{
	if (!mask)
		return;

	if (mask & CONTEXT_NOTI_LOCATION)
		update_location();

	if (mask & CONTEXT_NOTI_CONTACTS)
		update_contacts();

	if (mask & CONTEXT_NOTI_APPS)
		update_apps();

	if (mask & CONTEXT_NOTI_MUSIC)
		update_music();

	if (mask & CONTEXT_NOTI_PHOTOS)
		update_photo();

	if (mask & CONTEXT_NOTI_KEYWORD)
		update_keyword();

	return;
}

static bool ctx_changed_cb(context_type_e type, void *user_data)
{
	s_info.pending_mask |= type;
	if (!s_info.enabled || (client_count() && client_is_all_paused()) || setting_is_lcd_off())
		return ECORE_CALLBACK_RENEW;

	ctx_update(s_info.pending_mask);
	s_info.pending_mask = 0x0;
	return false;
}

static void ctx_vconf_cb(keynode_t *node, void *data)
{
	if (!node) {
		if (vconf_get_int(SYS_CLUSTER_KEY, &s_info.enabled) < 0) {
			s_info.enabled = 1; /*!< Enable this for default option */
		}
	} else {
		s_info.enabled = vconf_keynode_get_int(node);
	}
}

static Eina_Bool delayed_ctx_init_cb(void *data)
{
	context_set_context_changed_cb(ctx_changed_cb,
		CONTEXT_NOTI_LOCATION | CONTEXT_NOTI_CONTACTS | CONTEXT_NOTI_APPS |
		CONTEXT_NOTI_MUSIC | CONTEXT_NOTI_PHOTOS | CONTEXT_NOTI_KEYWORD, NULL);

	/*!
	 * Triggering all events first
	 */
	if (s_info.enabled)
		ctx_update(0xFFFFFFFF);

	return ECORE_CALLBACK_CANCEL;
}

int ctx_client_init(void)
{
	int ret;

	ret = vconf_notify_key_changed(SYS_CLUSTER_KEY, ctx_vconf_cb, NULL);
	if (ret < 0)
		ErrPrint("Failed to register the system_cluster vconf\n");

	ctx_vconf_cb(NULL, NULL);

	if (!ecore_timer_add(g_conf.delayed_ctx_init_time, delayed_ctx_init_cb, NULL)) {
		ErrPrint("Failed to add timer for delayed ctx init\n");
		delayed_ctx_init_cb(NULL);
	}

	return 0;
}

int ctx_client_fini(void)
{
	vconf_ignore_key_changed(SYS_CLUSTER_KEY, ctx_vconf_cb);
	return 0;
}

/* End of a file */
