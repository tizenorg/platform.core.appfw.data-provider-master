/*!
 * Managing the reference counter of a slave
 */

struct slave_node;

enum slave_event {
	SLAVE_EVENT_ACTIVATE,
	SLAVE_EVENT_DEACTIVATE, /* deactivate callback, can return REACTIVATE, DEFAULT */
	SLAVE_EVENT_DELETE,

	SLAVE_NEED_TO_REACTIVATE,
};

extern struct slave_node *slave_ref(struct slave_node *slave);
extern struct slave_node *slave_unref(struct slave_node *slave);
extern const int const slave_refcnt(struct slave_node *slave);

/*!
 * Create a new slave object or destroy it
 */
extern struct slave_node *slave_create(const char *name, int is_secured, const char *abi);
extern void slave_destroy(struct slave_node *slave);

/*!
 * Launch or terminate a slave
 */
extern int slave_activate(struct slave_node *slave);
extern int slave_deactivate(struct slave_node *slave);

/*!
 * To check the slave's activation state
 */
extern const int const slave_is_activated(struct slave_node *slave);
extern int slave_activated(struct slave_node *slave);

/*!
 * To mangage the unexpected termination of a slave
 */
extern void slave_deactivated_by_fault(struct slave_node *slave);

extern int slave_event_callback_add(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data);
extern int slave_event_callback_del(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data);

extern int slave_set_data(struct slave_node *slave, const char *tag, void *data);
extern void *slave_del_data(struct slave_node *slave, const char *tag);
extern void *slave_data(struct slave_node *slave, const char *tag);

extern void slave_faulted(struct slave_node *slave);
extern void slave_reset_fault(struct slave_node *slave);
extern const int const slave_is_faulted(const struct slave_node *slave);

extern struct slave_node *slave_find_by_pid(pid_t pid);
extern struct slave_node *slave_find_by_name(const char *name);

extern void slave_dead_handler(struct slave_node *slave);
extern void slave_handle_state_change(void);
extern const int const slave_is_secured(const struct slave_node *slave);
extern const char * const slave_name(const struct slave_node *slave);
extern const pid_t const slave_pid(const struct slave_node *slave);

/*!
 * \note
 * Used for making decision of destroying a slave or not
 * Used for balancing load of the slave.
 */
extern void slave_load_package(struct slave_node *slave);
extern void slave_unload_package(struct slave_node *slave);
extern struct slave_node *slave_find_available(void);

/*!
 * \note
 * Used for making decision of activating a slave or not
 */
extern void slave_load_instance(struct slave_node *slave);
extern void slave_unload_instance(struct slave_node *slave);
extern int const slave_loaded_instance(struct slave_node *slave);

extern int slave_resume(struct slave_node *slave);
extern int slave_pause(struct slave_node *slave);

/* End of a file */
