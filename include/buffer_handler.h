struct buffer_info;

enum buffer_type { /*!< Must have to be sync with libprovider, liblivebox-viewer */
	BUFFER_TYPE_FILE,
	BUFFER_TYPE_SHM,
	BUFFER_TYPE_PIXMAP,
	BUFFER_TYPE_ERROR,
};

/*!
 * \brief
 * \param[in] type
 * \param[in] w
 * \param[in] h
 * \param[in] pixel_size
 * \return buffer_info
 */
extern struct buffer_info *buffer_handler_create(enum buffer_type type, int w, int h, int pixel_size);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_destroy(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_load(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_unload(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_is_loaded(const struct buffer_info *info);

/*!
 * \brief Reallocate buffer
 * \param[in] info
 * \param[in] w
 * \param[in] h
 * \return int
 */
extern int buffer_handler_resize(struct buffer_info *info, int w, int h);

/*!
 * \brief Only update the size information
 * \param[in] info
 * \param[in] w
 * \param[in] h
 * \return void
 */
extern void buffer_handler_update_size(struct buffer_info *info, int w, int h);

/*!
 * \brief
 * \param[in] info
 * \return const char *
 */
extern const char *buffer_handler_id(const struct buffer_info *info);

/*!
 * \param[in] info
 * \return buffer_type
 */
extern enum buffer_type buffer_handler_type(const struct buffer_info *info);

/*!
 * \brief This API is not supported for Pixmap.
 * \param[in] info
 * \return void*
 */
extern void *buffer_handler_fb(const struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \param[out] w
 * \param[out] h
 * \return int
 */
extern int buffer_handler_get_size(struct buffer_info *info, int *w, int *h);

/*!
 * \brief This API only can be used for file type buffer
 * \param[in] info
 * \return void
 */
extern void buffer_handler_flush(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return 0 if fails. Return value should be casted to Pixmap type
 */
extern int buffer_handler_pixmap(const struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return buffer
 */
extern void *buffer_handler_pixmap_acquire_buffer(const struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_pixmap_release_buffer(const struct buffer_info *info);

/*!
 * \brief
 * \return int
 */
extern int buffer_handler_init(void);

/*!
 * \brief
 * \return int
 */
extern int buffer_handler_fini(void);

/* End of a file */
