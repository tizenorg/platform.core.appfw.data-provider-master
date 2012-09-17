struct liveinfo;

extern int liveinfo_init(void);
extern int liveinfo_fini(void);
extern struct liveinfo *liveinfo_create(pid_t pid, int handle);
extern int liveinfo_destroy(struct liveinfo *info);

extern struct liveinfo *liveinfo_find_by_pid(pid_t pid);
extern struct liveinfo *liveinfo_find_by_handle(int handle);

extern const char *liveinfo_filename(struct liveinfo *info);
extern pid_t liveinfo_pid(struct liveinfo *info);
extern FILE *liveinfo_fifo(struct liveinfo *info);
extern int liveinfo_open_fifo(struct liveinfo *info);
extern int liveinfo_close_fifo(struct liveinfo *info);

/* End of a file */
