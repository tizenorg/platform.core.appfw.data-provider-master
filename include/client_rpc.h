/*!
 */
extern int client_rpc_async_request(struct client_node *client, const char *funcname, GVariant *param);
extern int client_rpc_sync_request(struct client_node *client, const char *funcname, GVariant *param);
extern int client_rpc_broadcast(const char *funcname, GVariant *param);

/*!
 */
extern int client_rpc_update_proxy(struct client_node *client, GDBusProxy *proxy);
extern int client_rpc_update_conn(struct client_node *client, GDBusConnection *conn);

/*!
 */
extern struct client_node *client_rpc_find_by_proxy(GDBusProxy *proxy);
extern struct client_node *client_rpc_find_by_conn(GDBusConnection *conn);

/*!
 */
extern int client_rpc_initialize(struct client_node *client);

/* End of a file */
