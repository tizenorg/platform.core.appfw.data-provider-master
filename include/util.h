extern unsigned long util_string_hash(const char *str);
extern double util_get_timestamp(void);
extern int util_check_ext(const char *filename, const char *check_ptr);
extern int util_validate_livebox_package(const char *pkgname);
extern int util_unlink(const char *filename);
extern char *util_new_filename(double timestamp);

/* End of a file */
