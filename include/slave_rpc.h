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

extern int slave_rpc_async_request(struct slave_node *slave, const char *pkgname, struct packet *packet, void (*ret_cb)(struct slave_node *slave, const struct packet *packet, void *data), void *data, int urgent);
extern int slave_rpc_request_only(struct slave_node *slave, const char *pkgname, struct packet *packet, int urgent);

extern int slave_rpc_update_handle(struct slave_node *slave, int handle, int delete_pended_created_packet);
extern int slave_rpc_ping(struct slave_node *slave);
extern void slave_rpc_request_update(const char *pkgname, const char *id, const char *cluster, const char *category, const char *content, int force);
extern int slave_rpc_handle(struct slave_node *slave);
extern int slave_rpc_ping_freeze(struct slave_node *slave);
extern int slave_rpc_ping_thaw(struct slave_node *slave);

extern int slave_rpc_disconnect(struct slave_node *slave);

extern int slave_rpc_init(struct slave_node *slave);
extern int slave_rpc_fini(struct slave_node *slave);

/* End of a file */
