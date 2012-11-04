/*!
 */
extern int client_rpc_async_request(struct client_node *client, struct packet *packet);
extern int client_rpc_handle(struct client_node *client);

/*!
 */
extern int client_rpc_init(struct client_node *client, int handle);
extern int client_rpc_fini(struct client_node *client);

/* End of a file */
