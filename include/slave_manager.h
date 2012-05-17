struct slave_node;

extern struct slave_node *slave_find(const char *name);
extern struct slave_node *slave_find_usable(void);
extern struct slave_node *slave_find_by_pid(pid_t pid);
extern int slave_is_activated(struct slave_node *data);
extern int slave_activate(struct slave_node *data);
extern struct slave_node *slave_create(const char *name, int secured);
extern int slave_destroy(struct slave_node *data);
extern int slave_update_proxy(struct slave_node *data, GDBusProxy *proxy);
extern GDBusProxy *slave_get_proxy(struct slave_node *data);
extern int slave_ref(struct slave_node *slave);
extern int slave_unref(struct slave_node *slave);
extern int slave_refcnt(struct slave_node *slave);
extern int slave_broadcast_command(const char *cmd, GVariant *param);
extern int slave_push_command(struct slave_node *node, const char *pkgname, const char *filename, const char *cmd, GVariant *param, void (*ret_cb)(const char *funcname, GVariant *param, int ret, void *data), void *data);
extern const char *slave_name(struct slave_node *slave);
extern int slave_get_fault_count(struct slave_node *node);

extern int slave_manager_init(void);
extern int slave_manager_fini(void);

extern int slave_add_deactivate_cb(int (*cb)(struct slave_node *, void *), void *data);
extern void *slave_del_deactivate_cb(int (*cb)(struct slave_node *, void *));

extern int slave_fault_deactivating(struct slave_node *data, int terminate);
extern void slave_ping(struct slave_node *slave);
extern void slave_bye_bye(struct slave_node *slave);
extern void slave_set_pid(struct slave_node *slave, pid_t pid);
extern int slave_pid(struct slave_node *slave);
extern void slave_pause(struct slave_node *slave);
extern void slave_resume(struct slave_node *slave);
extern void slave_check_pause_or_resume(void);
extern int slave_is_secured(struct slave_node *slave);
/* End of a file */
