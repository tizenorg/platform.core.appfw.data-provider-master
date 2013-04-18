struct tcb;
struct service_context;

extern int tcb_fd(struct tcb *tcb);
extern struct service_context *tcb_svc_ctx(struct tcb *tcb);
extern int tcb_client_type(struct tcb *tcb);

extern struct service_context *service_common_create(const char *addr, int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data), void *data);
extern int service_common_destroy(struct service_context *svc_ctx);

extern int service_common_multicast_packet(struct tcb *tcb, struct packet *packet, int type);
extern int service_common_unicast_packet(struct tcb *tcb, struct packet *packet);

/* End of a file */
