extern int fault_check_pkgs(struct slave_node *node);
extern int fault_func_call(struct slave_node *node, const char *pkgname, const char *filename, const char *func);
extern int fault_func_ret(struct slave_node *node, const char *pkgname, const char *filename, const char *func);
extern int fault_is_occured(void);
extern void fault_unicast_info(struct client_node *client, const char *pkgname, const char *filename, const char *func);
extern void fault_broadcast_info(const char *pkgname, const char *filename, const char *func);

/* End of a file */
