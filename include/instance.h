enum instance_state {
	INST_DEACTIVATED = 0x0, /*!< Only keeps in the master */

	INST_REQUEST_TO_ACTIVATE, /*!< Load this to the slave */
	INST_ACTIVATED, /*!< This is loaded to the slave */

	INST_REQUEST_TO_DEACTIVATE, /*!< Unload this from the slave */

	INST_DESTROY, /*!< Delete this instance */
	INST_DESTROYED, /*!< Deleted by the slave automatically */
};

struct inst_info;
struct pkg_info;
struct script_handle;

extern struct inst_info *instance_create(struct client_node *client, double timestamp, const char *pkgname, const char *content, const char *cluster, const char *category, double period);
extern int instance_destroy(struct inst_info *inst);

extern struct inst_info * instance_ref(struct inst_info *inst);
extern struct inst_info * instance_unref(struct inst_info *inst);

extern void instance_set_state(struct inst_info *inst, enum instance_state state);

extern struct inst_info *instance_find_by_id(const char *pkgname, const char *id);
extern struct inst_info *instance_find_by_timestamp(const char *pkgname, double timestamp);

extern int instance_deactivate(struct inst_info *inst);
extern int instance_reactivate(struct inst_info *inst);
extern int instance_deactivated(struct inst_info *inst);
extern int instance_activate(struct inst_info *inst);

extern void instance_set_lb_info(struct inst_info *inst, int w, int h, double priority);
extern void instance_set_pd_info(struct inst_info *inst, int w, int h);

extern void instance_pd_updated(const char *pkgname, const char *id, const char *descfile);
extern void instance_lb_updated(const char *pkgname, const char *id);
extern void instance_lb_updated_by_instance(struct inst_info *inst);
extern void instance_pd_updated_by_instance(struct inst_info *inst, const char *descfile);

extern int instance_destroyed(struct inst_info *inst);

extern int instance_set_pinup(struct inst_info *inst, int pinup);
extern int instance_resize(struct inst_info *inst, int w, int h);
extern int instance_set_period(struct inst_info *inst, double period);
extern int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y);
extern int instance_text_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey);
extern int instance_change_group(struct inst_info *inst, const char *cluster, const char *category);

/*!
 * \note
 * getter
 */
extern double const instance_timestamp(struct inst_info *inst);
extern struct pkg_info * const instance_package(struct inst_info *inst);
extern struct script_info * const instance_lb_handle(struct inst_info *inst);
extern struct script_info * const instance_pd_handle(struct inst_info *inst);
extern char * const instance_id(struct inst_info *inst);
extern char * const instance_content(struct inst_info *inst);
extern char * const instance_category(struct inst_info *inst);
extern char * const instance_cluster(struct inst_info *inst);
extern int const instance_auto_launch(struct inst_info *inst);
extern int const instance_priority(struct inst_info *inst);
extern struct client_node * const instance_client(struct inst_info *inst);
extern double const instance_period(struct inst_info *inst);
extern int const instance_lb_width(struct inst_info *inst);
extern int const instance_lb_height(struct inst_info *inst);
extern int const instance_pd_width(struct inst_info *inst);
extern int const instance_pd_height(struct inst_info *inst);
extern enum instance_state const instance_state(struct inst_info *inst);

/*!
 * event
 */
extern int instance_unicast_created_event(struct inst_info *inst, struct client_node *client);
extern int instance_broadcast_created_event(struct inst_info *inst);
extern int instance_broadcast_deleted_event(struct inst_info *inst);
extern int instance_unicast_deleted_event(struct inst_info *inst);

/*!
 * Call this after got the size of lb or pd buffer
 */
extern int instance_prepare_fb(struct inst_info *inst);

/* End of a file */
