extern unsigned long util_string_hash(const char *str);
extern double util_timestamp(void);
extern int util_check_ext(const char *filename, const char *check_ptr);
extern int util_validate_livebox_package(const char *pkgname);
extern int util_unlink(const char *filename);
extern char *util_slavename(void);
extern const char *util_basename(const char *name);
extern unsigned long util_free_space(const char *path);
extern char *util_replace_string(const char *src, const char *pattern, const char *replace);
extern const char *util_uri_to_path(const char *uri);
extern void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data);
extern void util_timer_interval_set(void *timer, double interval);

#define SCHEMA_FILE	"file://"
#define SCHEMA_PIXMAP	"pixmap://"
#define SCHEMA_SHM	"shm://"

/* End of a file */
