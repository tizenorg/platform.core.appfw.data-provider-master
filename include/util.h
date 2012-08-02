extern unsigned long util_string_hash(const char *str);
extern double util_timestamp(void);
extern int util_check_ext(const char *filename, const char *check_ptr);
extern int util_validate_livebox_package(const char *pkgname);
extern int util_unlink(const char *filename);
extern char *util_slavename(void);
extern const char *util_basename(const char *name);
extern unsigned long util_free_space(const char *path);

#define URI_TO_PATH(uri)	((uri) + 7)

/* End of a file */
