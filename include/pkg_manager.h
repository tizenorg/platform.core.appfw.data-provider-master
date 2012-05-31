struct slave_node;
struct client_node;
struct pkg_info;
struct category;
struct inst_info;

enum func_ret_type {
	DONE = 0x00,
	NEED_TO_SCHEDULE = 0x01, /* Schedule me again right after return. */
	OUTPUT_UPDATED = 0x02, /* Output file is generated */

	NEED_TO_CREATE = 0x01,
	NEED_TO_DESTROY = 0x01,
};

extern int pkgmgr_init(void);
extern int pkgmgr_fini(void);

extern int pkgmgr_set_fault(const char *pkgname, const char *filename, const char *funcname);
extern int pkgmgr_clear_fault(const char *pkgname);
extern int pkgmgr_is_fault(const char *pkgname);
extern int pkgmgr_get_fault(const char *pkgname, double *timestamp, const char **filename, const char **funcname);
extern struct slave_node *pkgmgr_slave(const char *pkgname);
extern int pkgmgr_set_slave(const char *pkgname, struct slave_node *slave);
extern int pkgmgr_renew_by_slave(struct slave_node *node, int (*cb)(struct slave_node *, struct inst_info *, void *), void *data);
extern int pkgmgr_renew_by_pkg(const char *pkgname, int (*cb)(struct slave_node *, struct inst_info *, void *), void *data);

extern struct inst_info *pkgmgr_find(const char *pkgname, const char *filename);

extern struct inst_info *pkgmgr_new(double timestamp, const char *pkgname, const char *filename, const char *content, const char *cluster, const char *category);
extern int pkgmgr_delete(struct inst_info *inst);

extern void pkgmgr_set_info(struct inst_info *inst, int w, int h, double priority);

extern const char *pkgmgr_name(struct inst_info *inst);

extern const char *pkgmgr_lb_path(struct inst_info *inst);
extern const char *pkgmgr_lb_group(struct inst_info *inst);
extern const char *pkgmgr_pd_path(struct inst_info *inst);
extern const char *pkgmgr_pd_group(struct inst_info *inst);
extern double pkgmgr_timestamp(struct inst_info *inst);
extern int pkgmgr_set_client(struct inst_info *inst, struct client_node *client);
extern struct client_node *pkgmgr_client(struct inst_info *inst);
extern int pkgmgr_timeout(struct inst_info *inst);
extern double pkgmgr_period(struct inst_info *inst);
extern void pkgmgr_set_period(struct inst_info *inst, double period);
extern const char *pkgmgr_filename(struct inst_info *info);
extern const char *pkgmgr_cluster(struct inst_info *info);
extern const char *pkgmgr_category(struct inst_info *info);
extern const char *pkgmgr_content(struct inst_info *inst);
extern const char *pkgmgr_name_by_info(struct pkg_info *info);
extern void *pkgmgr_lb_script(struct inst_info *inst);
extern void *pkgmgr_pd_script(struct inst_info *inst);
extern const char *pkgmgr_abi(struct inst_info *inst);

extern int pkgmgr_delete_by_client(struct client_node *client);
extern int pkgmgr_delete_by_slave(struct slave_node *slave);

extern int pkgmgr_lb_updated(const char *pkgname, const char *filename, int w, int h, double priority);
extern int pkgmgr_pd_updated(const char *pkgname, const char *filename, const char *descfile, int w, int h);
extern int pkgmgr_created(const char *pkgname, const char *filename);
extern int pkgmgr_deleted(const char *pkgname, const char *filename);
extern void pkgmgr_lb_updated_by_inst(struct inst_info *inst);
extern void pkgmgr_pd_updated_by_inst(struct inst_info *inst, const char *descfile);
extern int pkgmgr_set_pinup(struct inst_info *inst, int flag);
extern int pkgmgr_pinup(struct inst_info *inst);

extern int pkgmgr_unload_pd(struct inst_info *inst);
extern int pkgmgr_load_pd(struct inst_info *inst);
extern int pkgmgr_is_secured(const char *pkgname);
extern int pkgmgr_inform_pkglist(struct client_node *client);
extern const char *pkgmgr_find_by_slave(struct slave_node *slave);

extern int pkgmgr_text_pd(struct inst_info *inst);
extern int pkgmgr_text_lb(struct inst_info *inst);

extern const char *pkgmgr_script(struct inst_info *inst);
extern int pkgmgr_update_size(struct inst_info *inst, int w, int h, int is_pd);
extern int pkgmgr_get_size(struct inst_info *inst, int *w, int *h, int is_pd);

extern void pkgmgr_clear_slave_info(struct slave_node *slave);

/* End of a file */
