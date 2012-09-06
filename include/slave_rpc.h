extern int slave_rpc_async_request(struct slave_node *slave, const char *pkgname, struct packet *packet, void (*ret_cb)(struct slave_node *slave, const struct packet *packet, void *data), void *data);
extern int slave_rpc_request_only(struct slave_node *slave, const char *pkgname, struct packet *packet);
extern int slave_rpc_update_handle(struct slave_node *slave, int handle);
extern int slave_rpc_ping(struct slave_node *slave);
extern void slave_rpc_request_update(const char *pkgname, const char *cluster, const char *category);
extern struct slave_node *slave_rpc_find_by_handle(int handle);
extern int slave_rpc_ping_freeze(struct slave_node *slave);
extern int slave_rpc_ping_thaw(struct slave_node *slave);

extern int slave_rpc_initialize(struct slave_node *slave);

/* End of a file */
