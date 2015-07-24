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

enum client_event {
	CLIENT_EVENT_ACTIVATE,
	CLIENT_EVENT_DEACTIVATE
};

enum client_global_event {
	CLIENT_GLOBAL_EVENT_CREATE,
	CLIENT_GLOBAL_EVENT_DESTROY
};

struct inst_info;
struct packet;

/*!
 * \note
 * Create & Destroy
 */
extern struct client_node *client_create(pid_t pid, int handle, const char *direct_addr);
#define client_destroy(client)	client_unref(client)

/*!
 * \note
 * Reference count
 */
extern struct client_node *client_ref(struct client_node *client);
extern struct client_node *client_unref(struct client_node *client);
extern const int const client_refcnt(const struct client_node *client);

/*!
 * \note
 * Information of client PID
 */
extern const pid_t const client_pid(const struct client_node *client);
extern struct client_node *client_find_by_pid(pid_t pid);
extern struct client_node *client_find_by_rpc_handle(int handle);

/*!
 * \note
 * Statistics for state of client
 */
extern const int const client_count_paused(void);
extern int client_is_all_paused(void);
extern int client_count(void);

/*!
 * \note
 * For dead signal handler
 */
extern struct client_node *client_deactivated_by_fault(struct client_node *client);
extern void client_reset_fault(struct client_node *client);
extern const int const client_is_faulted(const struct client_node *client);

/*!
 * \note
 * For other components which wants to know the state of a client
 */
extern int client_event_callback_add(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data);
extern int client_event_callback_del(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data);

extern int client_global_event_handler_del(enum client_global_event event_type, int (*cb)(struct client_node *, void *), void *data);
extern int client_global_event_handler_add(enum client_global_event event_type, int (*cb)(struct client_node *client, void *data), void *data);

/*!
 * \note
 * Private data set & get
 */
extern int client_set_data(struct client_node *client, const char *tag, void *data);
extern void *client_data(struct client_node *client, const char *tag);
extern void *client_del_data(struct client_node *client, const char *tag);

/*!
 * Handling the client statues
 * Paused or Resumed
 */
extern void client_paused(struct client_node *client);
extern void client_resumed(struct client_node *client);

/* Related with Context-Aware service */
extern int client_subscribe_group(struct client_node *client, const char *cluster, const char *category);
extern int client_unsubscribe_group(struct client_node *client, const char *cluster, const char *category);
extern int client_is_subscribed_group(struct client_node *client, const char *cluster, const char *category);

/* Related with category */
extern int client_subscribe_category(struct client_node *client, const char *category);
extern int client_unsubscribe_category(struct client_node *client, const char *category);
extern int client_is_subscribed_by_category(struct client_node *client, const char *category);

extern int client_init(void);
extern void client_fini(void);

extern int client_browse_group_list(const char *cluster, const char *category, int (*cb)(struct client_node *client, void *data), void *data);
extern int client_count_of_group_subscriber(const char *cluster, const char *category);

extern int client_browse_category_list(const char *category, int (*cb)(struct client_node *client, void *data), void *data);

extern int client_broadcast(struct inst_info *inst, struct packet *packet);

extern const char *client_direct_addr(const struct client_node *client);
extern struct client_node *client_find_by_direct_addr(const char *direct_addr);
extern void client_set_direct_fd(struct client_node *client, int fd);
extern int client_direct_fd(struct client_node *client);

extern int client_orientation(const struct client_node *client);
extern void client_set_orientation(struct client_node *client, int orientation);

extern const char *client_appid(const struct client_node *client);
extern int client_is_sdk_viewer(const struct client_node *client);
/* End of a file */
