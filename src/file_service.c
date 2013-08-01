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

#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <Eina.h>

#include <dlog.h>

#include <livebox-errno.h>
#include <packet.h>
#include <com-core.h>

#include "file_service.h"
#include "service_common.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

#define FILE_SERVICE_ADDR	"remote://:8209"
#define FILE_PUSH_ADDR		"remote://:8210"

#define PUSH_EXIT	'e'
#define PUSH_ITEM	'i'

#define PKT_CHUNKSZ	4096

static struct info {
	struct service_context *svc_ctx;

	pthread_t push_thid;

	Eina_List *request_list;
	pthread_mutex_t request_list_lock;

	int request_pipe[PIPE_MAX];
} s_info = {
	.svc_ctx = NULL,
	.request_list = NULL,
	.request_list_lock = PTHREAD_MUTEX_INITIALIZER,
	.request_pipe = { 0, },
};

struct request_item {
	char *filename;
	struct tcb *tcb;
};

struct burst_head {
	off_t size;
	int flen;
	char fname[];
};

struct burst_data {
	int size;
	char data[];
};

/* SERVER THREAD */
static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	const char *cmd;
	struct packet *reply;
	int ret;
	struct request_item *item;

	if (!packet) {
		DbgPrint("TCB %p is disconnected\n", tcb);
		return LB_STATUS_SUCCESS;
	}

	cmd = packet_command(packet);
	if (!cmd) {
		ErrPrint("Invalid packet. cmd is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	switch (packet_type(packet)) {
	case PACKET_REQ:
		item = NULL;

		if (strcmp(cmd, "request,file")) {
			const char *filename;
			if (packet_get(packet, "s", &filename) != 1) {
				ErrPrint("Invalid packet\n");
				ret = LB_STATUS_ERROR_INVALID;
			} else {
				item = malloc(sizeof(*item));
				if (!item) {
					ErrPrint("Heap: %s\n", strerror(errno));
					ret = LB_STATUS_ERROR_MEMORY;
				} else {
					item->filename = strdup(filename);
					if (!item->filename) {
						ErrPrint("Heap: %s\n", strerror(errno));
						free(item);
						ret = LB_STATUS_ERROR_MEMORY;
					} else {
						item->tcb = tcb;

						CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
						s_info.request_list = eina_list_append(s_info.request_list, item);
						CRITICAL_SECTION_END(&s_info.request_list_lock);

						ret = LB_STATUS_SUCCESS;
					}
				}
			}
		} else {
			ErrPrint("Unknown command\n");
			ret = LB_STATUS_ERROR_INVALID;
		}

		reply = packet_create_reply(packet, "i", ret);
		if (service_common_unicast_packet(tcb, reply) < 0)
			ErrPrint("Unable to send reply packet\n");
		packet_destroy(reply);

		/*!
		 * \note
		 * After send the reply packet, file push thread can sending a file
		 */
		if (item && ret == LB_STATUS_SUCCESS) {
			char ch = PUSH_ITEM;

			ret = write(s_info.request_pipe[PIPE_WRITE], &ch, sizeof(ch));
			if (ret < 0) {
				ErrPrint("write: %s\n", strerror(errno));

				CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
				s_info.request_list = eina_list_remove(s_info.request_list, item);
				CRITICAL_SECTION_END(&s_info.request_list_lock);

				free(item->filename);
				free(item);

				/*!
				 * \note
				 * In this case, the client can waiting files forever.
				 * So the client must has to wait only a few seconds.
				 * If the client could not get the any data in that time,
				 * it should cancel the waiting.
				 */
			}
		}

		/*!
		 * Protocol sequence
		 * FILE REQUEST COMMAND (Client -> Server)
		 * REPLY FOR REQUEST (Client <- Server)
		 * PUSH FILE (Client <- Server)
		 *
		 * Client & Server must has to keep this communication sequence.
		 */
		break;
	case PACKET_REQ_NOACK:
	case PACKET_ACK:
		/* File service has no this case, it is passive service type */
		ErrPrint("Invalid packet.\n");
		break;
	default:
		break;
	}

	return LB_STATUS_SUCCESS;
}

static inline int send_file(int conn_fd, const char *filename)
{
	struct burst_head *head;
	struct burst_data *body;
	int pktsz;
	int flen;
	off_t fsize;
	int fd;
	int ret = 0;

	/* TODO: push a file to the client */
	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		ErrPrint("open: %s\n", strerror(errno));
		return -EIO;
	}

	flen = strlen(filename);
	if (flen == 0) {
		ret = -EINVAL;
		goto errout;
	}

	pktsz = sizeof(*head) + flen + 1;

	head = malloc(pktsz);
	if (!head) {
		ErrPrint("heap: %s\n", strerror(errno));
		ret = -ENOMEM;
		goto errout;
	}

	fsize = lseek(fd, 0L, SEEK_END);
	if (fsize == (off_t)-1) {
		ErrPrint("heap: %s\n", strerror(errno));
		free(head);
		ret = -EIO;
		goto errout;
	}

	head->flen = flen;
	head->size = fsize;
	strcpy(head->fname, filename);

	/* Anytime we can fail to send packet */
	ret = com_core_send(conn_fd, (void *)head, pktsz, 2.0f);
	free(head);
	if (ret < 0) {
		ret = -EFAULT;
		goto errout;
	}

	if (lseek(fd, 0L, SEEK_SET) == (off_t)-1) {
		ErrPrint("seek: %s\n", strerror(errno));

		body = malloc(sizeof(*body));
		if (!body) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return -ENOMEM;
		}

		body->size = -1;
		ret = com_core_send(conn_fd, (void *)body, sizeof(*body), 2.0f);
		free(body);

		if (ret < 0)
			ret = -EFAULT;
		else
			ret = -EIO;

		goto errout;
	}

	body = malloc(PKT_CHUNKSZ + sizeof(*body));
	if (!body) {
		ErrPrint("heap: %s\n", strerror(errno));
		goto errout;
	}

	/* Burst pushing. */
	while (fsize > 0) {
		if (fsize > PKT_CHUNKSZ) {
			body->size = PKT_CHUNKSZ;
			fsize -= PKT_CHUNKSZ;
		} else {
			body->size = fsize;
			fsize = 0;
		}

		pktsz = sizeof(*body) + body->size;

		ret = read(fd, body->data, body->size); 
		if (ret < 0) {
			ErrPrint("read: %s\n", strerror(errno));
			ret = -EIO;
			break;
		}

		/* Send BODY */
		ret = com_core_send(conn_fd, (void *)body, pktsz, 2.0f);
		if (ret != pktsz) {
			ret = -EFAULT;
			break;
		}
	}

	/* Send EOF */
	body->size = -1;
	ret = com_core_send(conn_fd, (void *)body, sizeof(*body), 2.0f);
	if (ret < 0)
		ret = -EFAULT;

	free(body);

errout:
	if (close(fd) < 0)
		ErrPrint("close: %s\n", strerror(errno));

	return ret;
}

static void *push_main(void *data)
{
	fd_set set;
	int ret;
	char ch;
	struct request_item *item;
	int conn_fd;

	while (1) {
		FD_ZERO(&set);
		FD_SET(s_info.request_pipe[PIPE_READ], &set);
		ret = select(s_info.request_pipe[PIPE_READ] + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				ErrPrint("INTERRUPTED\n");
				ret = 0;
				continue;
			}
			ErrPrint("Error: %s\n", strerror(errno));
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout\n");
			ret = -ETIMEDOUT;
			break;
		}

		if (!FD_ISSET(s_info.request_pipe[PIPE_READ], &set)) {
			DbgPrint("Unknown data\n");
			ret = -EINVAL;
			break;
		}

		ret = read(s_info.request_pipe[PIPE_READ], &ch, sizeof(ch));
		if (ret != sizeof(ch)) {
			ErrPrint("read: %s\n", strerror(errno));
			ret = -EFAULT;
			break;
		}

		if (ch == PUSH_EXIT) {
			DbgPrint("Thread is terminating\n");
			ret = -ECANCELED;
			break;
		}

		CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
		item = eina_list_nth(s_info.request_list, 0);
		s_info.request_list = eina_list_remove(s_info.request_list, item);
		CRITICAL_SECTION_END(&s_info.request_list_lock);

		if (!item) {
			ErrPrint("Request item is not valid\n");
			ret = -EFAULT;
			break;
		}

		/* Validate the TCB? */
		conn_fd = tcb_is_valid(s_info.svc_ctx, item->tcb);
		if (conn_fd < 0) {
			ErrPrint("TCB is not valid\n");
			ret = -EINVAL;
			free(item->filename);
			free(item);
			break;
		}

		/*
		 * \note
		 * From now, we cannot believe the conn_fd.
		 * It can be closed any time.
		 * Even though we using it.
		 */
		(void)send_file(conn_fd, item->filename);

		free(item->filename);
		free(item);
	}

	return (void *)ret;
}

/* MAIN THREAD */
int file_service_init(void)
{
	int status;

	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return LB_STATUS_ERROR_ALREADY;
	}

	if (pipe2(s_info.request_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		ErrPrint("pipe: %s\n", strerror(errno));
		return LB_STATUS_ERROR_FAULT;
	}

	status = pthread_mutex_init(&s_info.request_list_lock, NULL);
	if (status != 0) {
		ErrPrint("Failed to create lock: %s\n", strerror(status));
		CLOSE_PIPE(s_info.request_pipe);
		return LB_STATUS_ERROR_FAULT;
	}

	s_info.svc_ctx = service_common_create(FILE_SERVICE_ADDR, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");

		status = pthread_mutex_destroy(&s_info.request_list_lock);
		if (status != 0)
			ErrPrint("Destroy lock: %s\n", strerror(status));

		CLOSE_PIPE(s_info.request_pipe);
		return LB_STATUS_ERROR_FAULT;
	}

	status = pthread_create(&s_info.push_thid, NULL, push_main, NULL);
	if (status != 0) {
		ErrPrint("Failed to create a push service: %s\n", strerror(status));

		service_common_destroy(s_info.svc_ctx);
		s_info.svc_ctx = NULL;

		status = pthread_mutex_destroy(&s_info.request_list_lock);
		if (status != 0)
			ErrPrint("Destroy lock: %s\n", strerror(status));

		CLOSE_PIPE(s_info.request_pipe);
		return LB_STATUS_ERROR_FAULT;
	}

	/*!
	 * \note
	 * Remote service doesn't need to set the additional SMAK label.
	 */

	DbgPrint("Successfully initiated\n");
	return LB_STATUS_SUCCESS;
}

/* MAIN THREAD */
int file_service_fini(void)
{
	struct request_item *item;
	int status;
	char ch;
	void *retval;

	if (!s_info.svc_ctx)
		return LB_STATUS_ERROR_INVALID;

	ch = PUSH_EXIT;
	status = write(s_info.request_pipe[PIPE_WRITE], &ch, sizeof(ch));
	if (status != sizeof(ch)) {
		ErrPrint("write: %s\n", strerror(errno));
		/* Forcely terminate the thread */
		status = pthread_cancel(s_info.push_thid);
		if (status != 0)
			ErrPrint("cancel: %s\n", strerror(status));
	}

	status = pthread_join(s_info.push_thid, &retval);
	if (status != 0)
		ErrPrint("join: %s\n", strerror(status));

	CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
	EINA_LIST_FREE(s_info.request_list, item) {
		free(item->filename);
		free(item);
	}
	CRITICAL_SECTION_END(&s_info.request_list_lock);

	service_common_destroy(s_info.svc_ctx);
	s_info.svc_ctx = NULL;

	status = pthread_mutex_destroy(&s_info.request_list_lock);
	if (status != 0)
		ErrPrint("destroy mutex: %s\n", strerror(status));

	CLOSE_PIPE(s_info.request_pipe);

	DbgPrint("Successfully Finalized\n");
	return LB_STATUS_SUCCESS;
}

/* End of a file */
