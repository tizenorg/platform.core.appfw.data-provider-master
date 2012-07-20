extern int critical_log(const char *func, int line, const char *fmt, ...);
extern int critical_log_init(void);
extern int critical_log_fini(void);

#define CRITICAL_LOG(args...) critical_log(__func__, __LINE__, args)

/* End of a file */
