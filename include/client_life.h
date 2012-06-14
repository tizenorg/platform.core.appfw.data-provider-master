enum client_event {
	CLIENT_EVENT_DEACTIVATE,
	CLIENT_EVENT_DESTROY,
};

enum client_global_event {
	CLIENT_GLOBAL_EVENT_CREATE,
	CLIENT_GLOBAL_EVENT_DESTROY,
};


/*!
 * \note
 * Create & Destroy
 */
extern struct client_node *client_create(pid_t pid);
extern int client_destroy(struct client_node *client);

/*!
 * \note
 * Reference count
 */
extern struct client_node *client_ref(struct client_node *client);
extern struct client_node *client_unref(struct client_node *client);
extern const int const client_refcnt(const struct client_node *client);

/*!
 * \note
 * Information of client PID
 */
extern const pid_t const client_pid(const struct client_node *client);
extern struct client_node *client_find_by_pid(pid_t pid);

/*!
 * \note
 * Statistics for state of client
 */
extern const int const client_count_paused(void);
extern int client_is_all_paused(void);
extern int client_count(void);

/*!
 * \note
 * For dead signal handler
 */
extern int client_deactivated_by_fault(struct client_node *client);
extern int client_fault(struct client_node *client);
extern void client_reset_fault(struct client_node *client);
extern const int const client_is_faulted(const struct client_node *client);

extern const int const client_is_activated(const struct client_node *client);

/*!
 * \note
 * For other components which wants to know the state of a client
 */
extern int client_event_callback_add(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data);
extern int client_event_callback_del(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data);

extern int client_global_event_handler_del(enum client_global_event event_type, int (*cb)(struct client_node *, void *), void *data);
extern int client_global_event_handler_add(enum client_global_event event_type, int (*cb)(struct client_node *client, void *data), void *data);

/*!
 * \note
 * Private data set & get
 */
extern int client_set_data(struct client_node *client, const char *tag, void *data);
extern void *client_data(struct client_node *client, const char *tag);
extern void *client_del_data(struct client_node *client, const char *tag);

/*!
 * Handling the client statues
 * Paused or Resumed
 */
extern void client_paused(struct client_node *client);
extern void client_resumed(struct client_node *client);

extern int client_init(void);
extern int client_fini(void);
/* End of a file */
