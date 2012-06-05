enum lb_type {
	LB_TYPE_NONE = 0x0,
	LB_TYPE_SCRIPT,
	LB_TYPE_FILE,
	LB_TYPE_TEXT,
};

enum pd_type {
	PD_TYPE_NONE = 0x0,
	PD_TYPE_SCRIPT,
	PD_TYPE_TEXT,
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
extern int package_dump_fault_info(struct pkg_info *info);
extern int package_set_fault_info(struct pkg_info *info, double timestamp, const char *filename, const char *function);

/*!
 * \brief
 * Readonly functions
 */
extern int const package_is_fault(struct pkg_info *info);
extern struct slave_node * const package_slave(struct pkg_info *info);
extern int const package_timeout(struct pkg_info *info);
extern double const package_period(struct pkg_info *info);
extern int const package_secured(struct pkg_info *info);
extern const char * const package_script(struct pkg_info *info);
extern const char * const package_abi(struct pkg_info *info);
extern const char * const package_lb_path(struct pkg_info *info);
extern const char * const package_lb_group(struct pkg_info *info);
extern const char * const package_pd_path(struct pkg_info *info);
extern const char * const package_pd_group(struct pkg_info *info);
extern int const package_pinup(struct pkg_info *info);
extern int const package_auto_launch(struct pkg_info *info);
extern unsigned int const package_size_list(struct pkg_info *info);
extern int const package_pd_width(struct pkg_info *info);
extern int const package_pd_height(struct pkg_info *info);

/*!
 * \brief
 * Reference counter
 */
extern struct pkg_info * const package_ref(struct pkg_info *info);
extern struct pkg_info * const package_unref(struct pkg_info *info);
extern int const package_refcnt(struct pkg_info *info);

extern enum pd_type package_pd_type(struct pkg_info *info);
extern enum lb_type package_lb_type(struct pkg_info *info);

extern int package_add_instance(struct pkg_info *info, struct inst_info *inst);
extern int package_del_instance(struct pkg_info *info, struct inst_info *inst);
extern Eina_List *package_instance_list(struct pkg_info *info);

extern int package_clear_fault(struct pkg_info *info);
extern int package_list_update(struct client_node *client);

extern int package_init(void);
extern int package_fini(void);

extern const char *package_name(struct pkg_info *info);
/* End of a file */
