/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
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

#include <Eina.h>

#include <dlog.h>
#include <livebox-errno.h>
#include <packet.h>

#include "service_common.h"
#include "debug.h"
#include "util.h"

#define SHORTCUT_ADDR	"/tmp/.shortcut.service"

/*!
 * SERVICE THREAD
 */
static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	DbgPrint("Command: [%s]\n", packet_command(packet));

	switch (packet_type(packet)) {
	case PACKET_REQ:
		/* Need to send reply packet */
		break;
	case PACKET_REQ_NOACK:
		/* Doesn't need to send reply packet */
		break;
	default:
		ErrPrint("Packet type is not valid\n");
		break;
	}

	/*!
	 * return value has no meanning,
	 * it will be printed by dlogutil.
	 */
	return 0;
}


/*!
 * MAIN THREAD
 * Do not try to do anyother operation in these functions
 */

static struct info {
	struct service_context *svc_ctx;
} s_info = {
	.svc_ctx = NULL,
};


int service_shortcut_init(void)
{
	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return LB_STATUS_ERROR_ALREADY;
	}

	s_info.svc_ctx = service_common_create(SHORTCUT_ADDR, service_thread_main, NULL);
	if (!s_info.svc_ctx)
		return LB_STATUS_ERROR_FAULT;

	return LB_STATUS_SUCCESS;
}

int service_shortcut_fini(void)
{
	if (!s_info.svc_ctx)
		return LB_STATUS_ERROR_INVALID;

	service_common_destroy(s_info.svc_ctx);
	return LB_STATUS_SUCCESS;
}

/* End of a file */
