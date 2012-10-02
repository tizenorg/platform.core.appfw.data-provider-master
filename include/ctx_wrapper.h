extern void ctx_wrapper_enable(void);
extern void ctx_wrapper_disable(void);
extern void *ctx_wrapper_register_callback(struct context_item *item, int (*cb)(struct context_item *item, void *user_data), void *user_data);
extern void *ctx_wrapper_unregister_callback(void *_cbfunc);

/* End of a file */
