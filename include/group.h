struct cluster;
struct category;
struct pkg_info;

extern struct cluster *group_create_cluster(const char *name);
extern struct cluster *group_find_cluster(const char *name);
extern int group_destroy_cluster(struct cluster *cluster);

extern struct category *group_create_category(struct cluster *cluster, const char *name);
extern struct category *group_find_category(struct cluster *cluster, const char *name);
extern int group_destroy_category(struct category *category);

extern const char * const group_category_name(struct category *category);
extern const char * const group_cluster_name(struct cluster *cluster);
extern const char *group_cluster_name_by_category(struct category *category);

extern int group_add_livebox(const char *group, const char *pkgname);
extern int group_del_livebox(const char *pkgname);

extern int group_list_category_pkgs(struct category *category, int (*cb)(struct category *category, const char *pkgname, void *data), void *data);

extern int group_init(void);
extern int group_fini(void);

/* End of a file */
