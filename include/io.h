extern int io_init(void);
extern int io_fini(void);
extern int io_load_package_db(struct pkg_info *info);
extern char *io_livebox_pkgname(const char *pkgname);
extern int io_update_livebox_package(const char *pkgname, int (*cb)(const char *lb_pkgname, int prime, void *data), void *data);

/* End of a file */
