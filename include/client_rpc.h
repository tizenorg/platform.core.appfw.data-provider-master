/*!
 */
extern int client_rpc_async_request(struct client_node *client, struct packet *packet);
extern int client_rpc_handle(struct client_node *client);

/*!
 */
extern int client_rpc_initialize(struct client_node *client, int handle);

/* End of a file */
