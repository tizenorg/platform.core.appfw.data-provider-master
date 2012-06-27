enum lb_type {
	LB_TYPE_NONE = 0x0,
	LB_TYPE_SCRIPT,
	LB_TYPE_FILE,
	LB_TYPE_TEXT,
	LB_TYPE_BUFFER,
};

enum pd_type {
	PD_TYPE_NONE = 0x0,
	PD_TYPE_SCRIPT,
	PD_TYPE_TEXT,
	PD_TYPE_BUFFER,
};

struct pkg_info;
struct inst_info;

/*!
 * \brief
 * Construction & Destruction
 */
extern struct pkg_info *package_create(const char *pkgname);
extern int package_destroy(struct pkg_info *info);
extern struct pkg_info *package_find(const char *pkgname);
extern const char *package_find_by_secured_slave(struct slave_node *slave);
extern struct inst_info *package_find_instance_by_id(const char *pkgname, const char *id);
extern struct inst_info *package_find_instance_by_timestamp(const char *pkgname, double timestamp);
extern int package_dump_fault_info(struct pkg_info *info);
extern int package_set_fault_info(struct pkg_info *info, double timestamp, const char *filename, const char *function);
extern int package_get_fault_info(struct pkg_info *info, double *timestmap, const char **filename, const char **function);

/*!
 * \brief
 * Readonly functions
 */
extern const int const package_is_fault(const struct pkg_info *info);
extern struct slave_node * const package_slave(const struct pkg_info *info);
extern const int const package_timeout(const struct pkg_info *info);
extern const double const package_period(const struct pkg_info *info);
extern const int const package_secured(const struct pkg_info *info);
extern const char * const package_script(const struct pkg_info *info);
extern const char * const package_abi(const struct pkg_info *info);
extern const char * const package_lb_path(const struct pkg_info *info);
extern const char * const package_lb_group(const struct pkg_info *info);
extern const char * const package_pd_path(const struct pkg_info *info);
extern const char * const package_pd_group(const struct pkg_info *info);
extern const int const package_pinup(const struct pkg_info *info);
extern const int const package_auto_launch(const struct pkg_info *info);
extern const unsigned int const package_size_list(const struct pkg_info *info);
extern const int const package_pd_width(const struct pkg_info *info);
extern const int const package_pd_height(const struct pkg_info *info);
extern const char * const package_name(const struct pkg_info *info);

/*!
 * \brief
 * Reference counter
 */
extern struct pkg_info * const package_ref(struct pkg_info *info);
extern struct pkg_info * const package_unref(struct pkg_info *info);
extern const int const package_refcnt(const struct pkg_info *info);

extern const enum pd_type const package_pd_type(const struct pkg_info *info);
extern const enum lb_type const package_lb_type(const struct pkg_info *info);

extern int package_add_instance(struct pkg_info *info, struct inst_info *inst);
extern int package_del_instance(struct pkg_info *info, struct inst_info *inst);
extern Eina_List *package_instance_list(struct pkg_info *info);

extern int package_clear_fault(struct pkg_info *info);
extern int package_list_update(struct client_node *client);

extern int package_init(void);
extern int package_fini(void);

/* End of a file */
