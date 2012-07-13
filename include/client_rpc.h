/*!
 */
extern int client_rpc_async_request(struct client_node *client, struct packet *packet);
extern int client_rpc_broadcast(struct inst_info *inst, struct packet *packet);

/*!
 */
extern struct client_node *client_rpc_find_by_handle(int handle);

/*!
 */
extern int client_rpc_initialize(struct client_node *client, int handle);

/* End of a file */
