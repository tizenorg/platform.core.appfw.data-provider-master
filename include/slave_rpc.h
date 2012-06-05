extern int slave_rpc_async_request(struct slave_node *slave,
			const char *pkgname, const char *filename,
			const char *cmd, GVariant *param, void (*ret_cb)(struct slave_node *slave, const char *func, GVariant *result, void *data),
			void *data);

extern int slave_rpc_sync_request(struct slave_node *slave,
			const char *pkgname, const char *filename,
			const char *cmd, GVariant *param);

extern int slave_rpc_pause_all(void);
extern int slave_rpc_resume_all(void);
extern int slave_rpc_ping(struct slave_node *slave);
extern int slave_rpc_update_proxy(struct slave_node *slave, GDBusProxy *proxy);
extern void slave_rpc_clear_request_by_slave(struct slave_node *slave);
extern void slave_rpc_request_update(const char *pkgname, const char *cluster, const char *category);

extern int slave_rpc_initialize(struct slave_node *slave);

/* End of a file */
