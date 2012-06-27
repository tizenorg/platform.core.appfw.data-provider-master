struct fb_info;

enum fb_type {
	FB_TYPE_UNKNWON,
	FB_TYPE_FILE,
	FB_TYPE_SHM,
};

extern int fb_init(void);
extern int fb_fini(void);
extern struct fb_info *fb_create(int w, int h, enum fb_type type);
extern int fb_destroy(struct fb_info *info);
extern Ecore_Evas * const fb_canvas(struct fb_info *info);
extern const char *fb_filename(struct fb_info *info);
extern void fb_get_size(struct fb_info *info, int *w, int *h);
extern void fb_sync(struct fb_info *info);
extern int fb_create_buffer(struct fb_info *info);
extern int fb_destroy_buffer(struct fb_info *info);
extern int fb_resize(struct fb_info *info, int w, int h);

/* End of a file */
