/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
enum tcb_type {
	TCB_CLIENT_TYPE_APP	= 0x00,
	TCB_CLIENT_TYPE_SERVICE	= 0x01,
	TCB_CLIENT_TYPE_UNKNOWN = 0xff
};

enum tcb_event_type {
	TCB_EVENT_CREATE = 0x01,
	TCB_EVENT_DESTROY = 0x02
};

struct tcb;
struct service_context;
struct service_event_item;

extern int tcb_fd(struct tcb *tcb);

/*!
 * \remarks This function will return valid pid only after it gets the packet from a client.
 *          or it will returns -1.
 * \param[in] Thread Control Block
 * \return pid Process Id
 * \retval -1 TCB is not valid or the client is remote host. so we cannot get the PID of it.
 * \retval >0 Process Id (PID)
 */
extern int tcb_pid(struct tcb *tcb);

extern struct service_context *tcb_svc_ctx(struct tcb *tcb);
extern int tcb_client_type(struct tcb *tcb);
extern int tcb_client_type_set(struct tcb *tcb, enum tcb_type type);
extern int tcb_is_valid(struct service_context *svc_ctx, struct tcb *tcb);

extern struct service_context *service_common_create(const char *addr, const char *label, int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data), void *data);
extern int service_common_destroy(struct service_context *svc_ctx);
extern int service_common_destroy_tcb(struct service_context *svc_ctx, struct tcb *tcb);

extern int service_common_multicast_packet(struct tcb *tcb, struct packet *packet, int type);
extern int service_common_unicast_packet(struct tcb *tcb, struct packet *packet);

extern struct service_event_item *service_common_add_timer(struct service_context *svc_ctx, double timer, int (*timer_cb)(struct service_context *svc_cx, void *data), void *data);
extern int service_common_update_timer(struct service_event_item *item, double timer);
extern int service_common_del_timer(struct service_context *svc_ctx, struct service_event_item *item);

extern int service_common_fd(struct service_context *ctx);

extern int service_register_tcb_callback(struct service_context *svc_ctx, struct tcb *tcb, enum tcb_event_type event, void (*cb)(struct service_context *svc_ctx, struct tcb *tcb, void *data), void *data);
extern int service_unregister_tcb_callback(struct service_context *svc_ctx, struct tcb *tcb, enum tcb_event_type event, void (*cb)(struct service_context *svc_ctx, struct tcb *tcb, void *data), void *data);

extern int service_common_send_packet_to_service(struct service_context *svc_ctx, struct tcb *tcb, struct packet *packet);

/* End of a file */
