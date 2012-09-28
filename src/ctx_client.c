#include <stdio.h>
#include <unistd.h>

#include <Ecore.h>
#include <gio/gio.h>

#include <vconf.h>
#include <dlog.h>

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
#include "ctx_wrapper.h"

static int ctx_changed_cb(struct context_item *item, void *user_data)
{
	const char *c_name;
	const char *s_name;
	const char *pkgname;
	struct context_info *info;
	struct category *category;

	if ((client_count() && client_is_all_paused()) || setting_is_lcd_off()) {
		ErrPrint("Context event is ignored\n");
		return 0;
	}

	info = group_context_info_from_item(item);
	category = group_category_from_context_info(info);

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);
	pkgname = group_pkgname_from_context_info(info);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid (%s/%s/%s)\n", c_name, s_name, pkgname);
		return 0;
	}

	slave_rpc_request_update(pkgname, "", c_name, s_name);
	if (util_free_space(IMAGE_PATH) > MINIMUM_SPACE) {
		double timestamp;
		struct inst_info *inst;

		timestamp = util_timestamp();
		inst = instance_create(NULL, timestamp, pkgname, DEFAULT_CONTENT, c_name, s_name, DEFAULT_PERIOD);
	} else {
		ErrPrint("Not enough space\n");
	}

	DbgPrint("Context event is updated\n");
	return 0;
}

static inline void register_callbacks(void)
{
	Eina_List *cluster_list;
	Eina_List *l1;
	struct cluster *cluster;

	Eina_List *category_list;
	Eina_List *l2;
	struct category *category;

	Eina_List *info_list;
	Eina_List *l3;
	struct context_info *info;

	Eina_List *item_list;
	Eina_List *l4;
	struct context_item *item;

	cluster_list = group_cluster_list();
	EINA_LIST_FOREACH(cluster_list, l1, cluster) {
		category_list = group_category_list(cluster);
		EINA_LIST_FOREACH(category_list, l2, category) {
			info_list = group_context_info_list(category);
			EINA_LIST_FOREACH(info_list, l3, info) {
				item_list = group_context_item_list(info);
				EINA_LIST_FOREACH(item_list, l4, item) {
					void *handler;
					handler = ctx_wrapper_register_callback(item, ctx_changed_cb, NULL);
					if (group_context_item_add_data(item, "callback", handler) < 0)
						ctx_wrapper_unregister_callback(handler);
				} // item
			} // info
		} // category
	} // cluster
}

static inline void unregister_callbacks(void)
{
	Eina_List *cluster_list;
	Eina_List *l1;
	struct cluster *cluster;

	Eina_List *category_list;
	Eina_List *l2;
	struct category *category;

	Eina_List *info_list;
	Eina_List *l3;
	struct context_info *info;

	Eina_List *item_list;
	Eina_List *l4;
	struct context_item *item;

	cluster_list = group_cluster_list();
	EINA_LIST_FOREACH(cluster_list, l1, cluster) {
		category_list = group_category_list(cluster);
		EINA_LIST_FOREACH(category_list, l2, category) {
			info_list = group_context_info_list(category);
			EINA_LIST_FOREACH(info_list, l3, info) {
				item_list = group_context_item_list(info);
				EINA_LIST_FOREACH(item_list, l4, item) {
					void *handler;
					handler = group_context_item_del_data(item, "callback");
					if (handler)
						ctx_wrapper_unregister_callback(handler);
				} // item
			} // info
		} // category
	} // cluster
}

static void ctx_vconf_cb(keynode_t *node, void *data)
{
	int enabled;

	if (!node) {
		/*!< Enable this for default option */
		if (vconf_get_int(SYS_CLUSTER_KEY, &enabled) < 0)
			enabled = 0;
	} else {
		enabled = vconf_keynode_get_int(node);
	}

	if (!enabled) {
		unregister_callbacks();
		ctx_wrapper_disable();
		return;
	}

	ctx_wrapper_enable();
	/*!
	 * Register event callbacks for every liveboxes
	 */
	register_callbacks();
}

int ctx_client_init(void)
{
	int ret;

	ret = vconf_notify_key_changed(SYS_CLUSTER_KEY, ctx_vconf_cb, NULL);
	if (ret < 0)
		ErrPrint("Failed to register the system_cluster vconf\n");

	ctx_vconf_cb(NULL, NULL);
	return 0;
}

int ctx_client_fini(void)
{
	vconf_ignore_key_changed(SYS_CLUSTER_KEY, ctx_vconf_cb);
	return 0;
}

/* End of a file */
