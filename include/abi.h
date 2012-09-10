extern int abi_add_entry(const char *abi, const char *pkgname);
extern int abi_update_entry(const char *abi, const char *pkgname);
extern int abi_del_entry(const char *abi);
extern const char *abi_find_slave(const char *abi);
extern int abi_del_all(void);
extern const char *abi_find_by_pkgname(const char *pkgname);

/* End of a file */
