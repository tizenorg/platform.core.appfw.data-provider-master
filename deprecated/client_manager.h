struct client_node;
struct pkg_info;

extern struct client_node *client_new(int pid);
extern struct client_node *client_find(int pid);
extern struct client_node *client_find_by_connection(GDBusConnection *conn);
extern int client_destroy(struct client_node *client);
extern int client_update_proxy(struct client_node *client, GDBusProxy *proxy);
extern int client_push_command(struct client_node *client, const char *funcname, GVariant *param);
extern int client_broadcast_command(const char *funcname, GVariant *param);
extern int client_pid(struct client_node *client);
extern GDBusProxy *client_proxy(struct client_node *client);

extern void client_pause(struct client_node *client);
extern void client_resume(struct client_node *client);
extern int client_is_all_paused(void);
extern int client_count(void);

/* End of a file */
