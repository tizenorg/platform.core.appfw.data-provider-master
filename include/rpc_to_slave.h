
extern int rpc_send_new(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *param, int ret, void *data), void *data, int skip_need_to_create);

extern void rpc_send_update_request(const char *pkgname, const char *cluster, const char *category);
extern struct inst_info *rpc_send_create_request(struct client_node *client, const char *pkgname, const char *content, const char *cluster, const char *category, double timestamp);
extern void rpc_send_resume_request(void);
extern void rpc_send_pause_request(void);

/* End of a file */
