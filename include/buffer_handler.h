struct buffer_info;

enum buffer_type { /*!< Must have to be sync with libprovider, liblivebox-viewer */
	BUFFER_TYPE_FILE,
	BUFFER_TYPE_SHM,
	BUFFER_TYPE_PIXMAP,
	BUFFER_TYPE_ERROR,
};

extern struct buffer_info *buffer_handler_create(enum buffer_type type, int w, int h, int pixel_size);
extern int buffer_handler_destroy(struct buffer_info *info);

extern int buffer_handler_load(struct buffer_info *info);
extern int buffer_handler_unload(struct buffer_info *info);
extern int buffer_handler_is_loaded(const struct buffer_info *info);

extern int buffer_handler_resize(struct buffer_info *info, int w, int h);
extern const char *buffer_handler_id(const struct buffer_info *info);
extern enum buffer_type buffer_handler_type(const struct buffer_info *info);
extern int buffer_handler_get_size(struct buffer_info *info, int *w, int *h);
extern void buffer_handler_flush(struct buffer_info *info);
extern void *buffer_handler_buffer(struct buffer_info *info);

extern int buffer_handler_init(void);
extern int buffer_handler_fini(void);

/* End of a file */
