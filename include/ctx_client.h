struct context_info;

extern int ctx_client_init(void);
extern int ctx_client_fini(void);
extern void ctx_update(void);
extern int ctx_enable_event_handler(struct context_info *info);
extern int ctx_disable_event_handler(struct context_info *info);

/* End of a file */
