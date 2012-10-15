struct fb_info;
struct inst_info;

extern int fb_init(void);
extern int fb_fini(void);
extern struct fb_info *fb_create(struct inst_info *inst, int w, int h, enum buffer_type type);
extern int fb_destroy(struct fb_info *info);
extern Ecore_Evas * const fb_canvas(struct fb_info *info);
extern const char *fb_id(struct fb_info *info);
extern int fb_get_size(struct fb_info *info, int *w, int *h);
extern void fb_sync(struct fb_info *info);
extern int fb_create_buffer(struct fb_info *info);
extern int fb_destroy_buffer(struct fb_info *info);
extern int fb_resize(struct fb_info *info, int w, int h);

/*!
 * \note Only for the pixmap
 */
extern void *fb_pixmap_render_pre(struct fb_info *info);
extern int fb_pixmap_render_post(struct fb_info *info);

/* End of a file */
