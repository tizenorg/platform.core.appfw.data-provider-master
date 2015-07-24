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

#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <Eina.h>

#include <dlog.h>

#include <widget_errno.h>
#include <widget_cmd_list.h>
#include <widget_service.h>
#include <packet.h>
#include <com-core_packet.h>

#include "shared_fd_service.h"
#include "service_common.h"
#include "client_life.h"
#include "client_rpc.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

static struct info {
	int fd;
} s_info = {
	.fd = -1,
};

static struct packet *direct_hello_handler(pid_t pid, int handle, const struct packet *packet)
{
	const char *direct_addr;
	struct client_node *client;

	if (!packet) {
		ErrPrint("%d is disconnected (%d)\n", pid, handle);
		return NULL;
	}

	if (packet_get(packet, "s", &direct_addr) != 1) {
		ErrPrint("Packet is not valid\n");
		return NULL;
	}

	client = client_find_by_direct_addr(direct_addr);
	if (!client) {
		ErrPrint("Client is not exists: %s\n", direct_addr);
		return NULL;
	}

	DbgPrint("Client Direct Handler is updated: %d\n", handle);
	client_set_direct_fd(client, handle);
	return NULL;
}

static struct packet *direct_connected_handler(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	const char *direct_addr;
	struct client_node *client;

	if (!packet) {
		ErrPrint("%d is disconnected (%d)\n", pid, handle);
		return NULL;
	}

	if (packet_get(packet, "s", &direct_addr) != 1) {
		ErrPrint("Packet is not valid\n");
		result = packet_create_reply(packet, "i", WIDGET_ERROR_INVALID_PARAMETER);
		return result;
	}

	client = client_find_by_direct_addr(direct_addr);
	if (!client) {
		ErrPrint("Client is not exists: %s\n", direct_addr);
		result = packet_create_reply(packet, "i", WIDGET_ERROR_NOT_EXIST);
		return result;
	}

	result = packet_create_reply(packet, "i", WIDGET_ERROR_NONE);
	packet_set_fd(result, client_direct_fd(client));
	DbgPrint("Set FD Handle for (%s): %d\n", direct_addr, client_direct_fd(client));
	return result;
}

static struct method s_table[] = {
	{
		.cmd = CMD_STR_DIRECT_HELLO,
		.handler = direct_hello_handler,
	},
	{
		.cmd = CMD_STR_DIRECT_CONNECTED,
		.handler = direct_connected_handler,
	},
	{
		.cmd = NULL,
		.handler = NULL,
	},
};

/**
 * @note
 * MAIN THREAD
 */
HAPI int shared_fd_service_init(void)
{
	DbgPrint("Successfully initiated\n");
	/**
	 * @todo
	 * 1. Accept FD (Get a new connection handle)
	 * 2. Waiting "hello" message from viewer.
	 * 3. Waiting "connected" message from provider
	 * 4. Send accepted FD to provider via result packet.
	 * 5. Provider will send a packet to the viewer via that FD.
	 */
	s_info.fd = com_core_packet_server_init("sdlocal://"SHARED_SOCKET, s_table);
	if (s_info.fd < 0) {
		ErrPrint("Failed to make a server for %s\n", "sdlocal://"SHARED_SOCKET);
	}

	return WIDGET_ERROR_NONE;
}

/**
 * @note
 * MAIN THREAD
 */
HAPI int shared_fd_service_fini(void)
{
	if (s_info.fd >= 0) {
		com_core_packet_server_fini(s_info.fd);
		s_info.fd = -1;
	}

	DbgPrint("Successfully Finalized\n");
	return WIDGET_ERROR_NONE;
}

/* End of a file */
