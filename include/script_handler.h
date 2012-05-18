struct script_info;

extern struct script_info *script_handler_create(struct inst_info *inst, const char *file, const char *group, int w, int h);
extern int script_handler_destroy(struct script_info *info);
extern struct fb_info *script_handler_fb(struct script_info *info);
extern void *script_handler_evas(struct script_info *info);
extern int script_handler_parse_desc(const char *pkgname, const char *filename, const char *descfile, int is_pd);
extern int script_handler_unload(struct script_info *info, int is_pd);
extern int script_handler_load(struct script_info *info, int is_pd);
extern int script_handler_is_loaded(struct script_info *info);

extern int script_init(void);
extern int script_fini(void);

extern int script_signal_emit(Evas *e, const char *part, const char *signal, double x, double y, double ex, double ey);

/* End of a file */
