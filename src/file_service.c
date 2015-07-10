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
#include <widget_service.h>
#include <packet.h>
#include <com-core.h>

#include "file_service.h"
#include "service_common.h"
#include "debug.h"
#include "util.h"
#include "conf.h"
#include "buffer_handler.h"

#define FILE_SERVICE_ADDR	"remote://:8209"

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
	enum {
		REQUEST_TYPE_FILE = 0x00,
		REQUEST_TYPE_SHM = 0x01,
		REQUEST_TYPE_PIXMAP = 0x02,
		REQUEST_TYPE_MAX = 0x02,
	} type;
	union {
		char *filename;
		int shm;
		unsigned int pixmap;
	} data;
	struct tcb *tcb;
};

typedef int (*send_data_func_t)(int fd, const struct request_item *item);

/*!
 * File transfer header.
 * This must should be shared with client.
 */
struct burst_head {
	off_t size;
	int flen;
	char fname[];
};

struct burst_data {
	int size;
	char data[];
};

static inline struct request_item *create_request_item(struct tcb *tcb, int type, void *data)
{
	struct request_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("malloc: %d\n", errno);
		return NULL;
	}

	switch (type) {
	case REQUEST_TYPE_FILE:
		item->data.filename = strdup(data);
		if (!item->data.filename) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(item);
			return NULL;
		}
		break;
	case REQUEST_TYPE_PIXMAP:
		item->data.pixmap = (unsigned int)((long)data);
		break;
	case REQUEST_TYPE_SHM:
		item->data.shm = (int)((long)data);
		break;
	default:
		ErrPrint("Invalid type of request\n");
		DbgFree(item);
		return NULL;
	}

	item->type = type;
	item->tcb = tcb;
	return item;
}

static inline int destroy_request_item(struct request_item *item)
{
	switch (item->type) {
	case REQUEST_TYPE_FILE:
		DbgFree(item->data.filename);
		break;
	case REQUEST_TYPE_SHM:
	case REQUEST_TYPE_PIXMAP:
		break;
	default:
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	DbgFree(item);
	return WIDGET_ERROR_NONE;
}

static int request_file_handler(struct tcb *tcb, struct packet *packet, struct request_item **item)
{
	const char *filename;

	if (packet_get(packet, "s", &filename) != 1) {
		ErrPrint("Invalid packet\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	*item = create_request_item(tcb, REQUEST_TYPE_FILE, (void *)filename);
	return *item ? WIDGET_ERROR_NONE : WIDGET_ERROR_OUT_OF_MEMORY;
}

static int request_pixmap_handler(struct tcb *tcb, struct packet *packet, struct request_item **item)
{
	unsigned long pixmap;

	if (packet_get(packet, "i", &pixmap) != 1) {
		ErrPrint("Invalid packet\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (pixmap == 0) {
		ErrPrint("pixmap is not valid\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	/*!
	 * \TODO
	 * Attach to pixmap and copy its data to the client
	 */
	*item = create_request_item(tcb, REQUEST_TYPE_PIXMAP, (void *)pixmap);
	return *item ? WIDGET_ERROR_NONE : WIDGET_ERROR_OUT_OF_MEMORY;
}

static int request_shm_handler(struct tcb *tcb, struct packet *packet, struct request_item **item)
{
	int shm;

	if (packet_get(packet, "i", &shm) != 1) {
		ErrPrint("Invalid packet\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (shm < 0) {
		ErrPrint("shm is not valid: %d\n", shm);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	/*!
	 * \TODO
	 * Attach to SHM and copy its buffer to the client
	 */
	*item = create_request_item(tcb, REQUEST_TYPE_SHM, (void *)((long)shm));
	return *item ? WIDGET_ERROR_NONE : WIDGET_ERROR_OUT_OF_MEMORY;
}

/* SERVER THREAD */
static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	const char *cmd;
	char ch = PUSH_ITEM;
	int ret;
	int i;
	struct request_item *item;
	struct packet *reply;
	struct {
		const char *cmd;
		int (*request_handler)(struct tcb *tcb, struct packet *packet, struct request_item **item);
	} cmd_table[] = {
		{
			.cmd = "request,file",
			.request_handler = request_file_handler,
		},
		{
			.cmd = "request,pixmap",
			.request_handler = request_pixmap_handler,
		},
		{
			.cmd = "request,shm",
			.request_handler = request_shm_handler,
		},
		{
			.cmd = NULL,
			.request_handler = NULL,
		},
	};

	if (!packet) {
		DbgPrint("TCB %p is disconnected\n", tcb);
		return WIDGET_ERROR_NONE;
	}

	cmd = packet_command(packet);
	if (!cmd) {
		ErrPrint("Invalid packet. cmd is not valid\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (packet_type(packet)) {
	case PACKET_REQ:
		for (i = 0; cmd_table[i].cmd; i++) {
			/*!
			 * Protocol sequence
			 * FILE REQUEST COMMAND (Client -> Server)
			 * REPLY FOR REQUEST (Client <- Server)
			 * PUSH FILE (Client <- Server)
			 *
			 * Client & Server must has to keep this communication sequence.
			 */
			if (strcmp(cmd, cmd_table[i].cmd)) {
				continue;
			}

			item = NULL;
			ret = cmd_table[i].request_handler(tcb, packet, &item);

			reply = packet_create_reply(packet, "i", ret);
			if (!reply) {
				ErrPrint("Failed to create a reply packet\n");
				break;
			}

			if (service_common_unicast_packet(tcb, reply) < 0) {
				ErrPrint("Unable to send reply packet\n");
			}

			packet_destroy(reply);

			/*!
			 * \note
			 * After send the reply packet, file push thread can sending a file
			 */
			if (ret != WIDGET_ERROR_NONE || !item) {
				break;
			}

			CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
			s_info.request_list = eina_list_append(s_info.request_list, item);
			CRITICAL_SECTION_END(&s_info.request_list_lock);

			ret = write(s_info.request_pipe[PIPE_WRITE], &ch, sizeof(ch));
			if (ret < 0) {
				ErrPrint("write: %d\n", errno);

				CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
				s_info.request_list = eina_list_remove(s_info.request_list, item);
				CRITICAL_SECTION_END(&s_info.request_list_lock);

				destroy_request_item(item);
				/*!
				 * \note for the client
				 * In this case, the client can waiting files forever.
				 * So the client must has to wait only a few seconds.
				 * If the client could not get the any data in that time,
				 * it should cancel the waiting.
				 */
			}
		}

		break;
	case PACKET_REQ_NOACK:
	case PACKET_ACK:
		/* File service has no this case, it is passive service type */
		ErrPrint("Invalid packet.\n");
		break;
	default:
		break;
	}

	return WIDGET_ERROR_NONE;
}

static int send_file(int handle, const struct request_item *item)
{
	struct burst_head *head;
	struct burst_data *body;
	int pktsz;
	int flen;
	off_t fsize;
	int fd;
	int ret = 0;

	/* TODO: push a file to the client */
	fd = open(item->data.filename, O_RDONLY);
	if (fd < 0) {
		ErrPrint("open: %d\n", errno);
		return -EIO;
	}

	flen = strlen(item->data.filename);
	if (flen == 0) {
		ret = -EINVAL;
		goto errout;
	}

	pktsz = sizeof(*head) + flen + 1;

	head = malloc(pktsz);
	if (!head) {
		ErrPrint("malloc: %d\n", errno);
		ret = -ENOMEM;
		goto errout;
	}

	fsize = lseek(fd, 0L, SEEK_END);
	if (fsize == (off_t)-1) {
		ErrPrint("lseek: %d\n", errno);
		DbgFree(head);
		ret = -EIO;
		goto errout;
	}

	head->flen = flen;
	head->size = fsize;
	strcpy(head->fname, item->data.filename);

	/* Anytime we can fail to send packet */
	ret = com_core_send(handle, (void *)head, pktsz, 2.0f);
	DbgFree(head);
	if (ret < 0) {
		ret = -EFAULT;
		goto errout;
	}

	if (lseek(fd, 0L, SEEK_SET) == (off_t)-1) {
		ErrPrint("lseek: %s\n", errno);

		body = malloc(sizeof(*body));
		if (!body) {
			ErrPrint("malloc: %d\n", errno);
			ret = -ENOMEM;
			goto errout;
		}

		body->size = -1;
		ret = com_core_send(handle, (void *)body, sizeof(*body), 2.0f);
		DbgFree(body);

		if (ret < 0) {
			ret = -EFAULT;
		} else {
			ret = -EIO;
		}

		goto errout;
	}

	body = malloc(PKT_CHUNKSZ + sizeof(*body));
	if (!body) {
		ErrPrint("malloc: %d\n", errno);
		goto errout;
	}

	/* Burst pushing. */
	while (fsize > 0) {
		if (fsize > PKT_CHUNKSZ) {
			body->size = PKT_CHUNKSZ;
		} else {
			body->size = fsize;
		}

		ret = read(fd, body->data, body->size); 
		if (ret < 0) {
			ErrPrint("read: %d\n", errno);
			break;
		}

		body->size = ret;
		fsize -= ret;
		pktsz = sizeof(*body) + body->size;

		/* Send BODY */
		ret = com_core_send(handle, (void *)body, pktsz, 2.0f);
		if (ret != pktsz) {
			break;
		}
	}

	/* Send EOF */
	body->size = -1;
	ret = com_core_send(handle, (void *)body, sizeof(*body), 2.0f);
	if (ret < 0) {
		ret = -EFAULT;
	}

	DbgFree(body);

errout:
	if (close(fd) < 0) {
		ErrPrint("close: %d\n", errno);
	}

	return ret;
}

static int send_buffer(int handle, const struct request_item *item)
{
	widget_fb_t buffer;
	struct burst_head *head;
	struct burst_data *body;
	char *data;
	int pktsz;
	int ret;
	int size;
	int offset;
	int type;

	if (item->type == REQUEST_TYPE_SHM) {
		type = WIDGET_FB_TYPE_SHM;
	} else {
		type = WIDGET_FB_TYPE_PIXMAP;
	}

	buffer = buffer_handler_raw_open(type, (void *)(long)item->data.shm);
	if (!buffer) {
		return -EINVAL;
	}

	pktsz = sizeof(*head);

	head = malloc(pktsz);
	if (!head) {
		ErrPrint("malloc: %d\n", errno);
		(void)buffer_handler_raw_close(buffer);
		return -ENOMEM;
	}

	size = head->size = buffer_handler_raw_size(buffer);
	head->flen = 0;

	/* Anytime we can fail to send packet */
	ret = com_core_send(handle, (void *)head, pktsz, 2.0f);
	DbgFree(head);
	if (ret < 0) {
		ret = -EFAULT;
		goto errout;
	}

	body = malloc(sizeof(*body) + PKT_CHUNKSZ);
	if (!body) {
		ret = -ENOMEM;
		goto errout;
	}

	data = (char *)buffer_handler_raw_data(buffer);
	offset = 0;
	while (offset < size) {
		body->size = size - offset;

		if (body->size > PKT_CHUNKSZ) {
			body->size = PKT_CHUNKSZ;
		}

		memcpy(body->data, data, body->size);
		pktsz = sizeof(*body) + body->size;

		ret = com_core_send(handle, (void *)body, pktsz, 2.0f);
		if (ret < 0) {
			ret = -EFAULT;
			break;
		}

		offset += body->size;
	}

	DbgFree(body);

errout:
	(void)buffer_handler_raw_close(buffer);
	return ret;
}

static void *push_main(void *data)
{
	fd_set set;
	int ret;
	char ch;
	struct request_item *item;
	int conn_fd;
	send_data_func_t send_data[] = {
		send_file,
		send_buffer,
		send_buffer,
	};

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
			ErrPrint("select: %d\n", errno);
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
			ErrPrint("read: %d\n", errno);
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
			continue;
		}

		/* Validate the TCB? */
		conn_fd = tcb_is_valid(s_info.svc_ctx, item->tcb);
		if (conn_fd < 0) {
			ErrPrint("TCB is not valid\n");
			destroy_request_item(item);
			continue;
		}

		/*
		 * \note
		 * From now, we cannot believe the conn_fd.
		 * It can be closed any time.
		 * Even though we using it.
		 */
		if (item->type < REQUEST_TYPE_MAX && item->type >= 0) {
			(void)send_data[item->type](conn_fd, item);
		} else {
			ErrPrint("Invalid type\n");
		}

		destroy_request_item(item);
	}

	return (void *)((long)ret);
}

/* MAIN THREAD */
int file_service_init(void)
{
	int status;

	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return WIDGET_ERROR_ALREADY_STARTED;
	}

	if (pipe2(s_info.request_pipe, O_CLOEXEC) < 0) {
		ErrPrint("pipe: %d\n", errno);
		return WIDGET_ERROR_FAULT;
	}

	status = pthread_mutex_init(&s_info.request_list_lock, NULL);
	if (status != 0) {
		ErrPrint("Failed to create lock: %d\n", status);
		CLOSE_PIPE(s_info.request_pipe);
		return WIDGET_ERROR_FAULT;
	}

	s_info.svc_ctx = service_common_create(FILE_SERVICE_ADDR, NULL, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");

		status = pthread_mutex_destroy(&s_info.request_list_lock);
		if (status != 0) {
			ErrPrint("Destroy lock: %d\n", status);
		}

		CLOSE_PIPE(s_info.request_pipe);
		return WIDGET_ERROR_FAULT;
	}

	status = pthread_create(&s_info.push_thid, NULL, push_main, NULL);
	if (status != 0) {
		ErrPrint("Failed to create a push service: %d\n", status);

		service_common_destroy(s_info.svc_ctx);
		s_info.svc_ctx = NULL;

		status = pthread_mutex_destroy(&s_info.request_list_lock);
		if (status != 0) {
			ErrPrint("Destroy lock: %d\n", status);
		}

		CLOSE_PIPE(s_info.request_pipe);
		return WIDGET_ERROR_FAULT;
	}

	/*!
	 * \note
	 * Remote service doesn't need to set the additional SMAK label.
	 */

	DbgPrint("Successfully initiated\n");
	return WIDGET_ERROR_NONE;
}

/* MAIN THREAD */
int file_service_fini(void)
{
	struct request_item *item;
	int status;
	char ch;
	void *retval;

	if (!s_info.svc_ctx) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	ch = PUSH_EXIT;
	status = write(s_info.request_pipe[PIPE_WRITE], &ch, sizeof(ch));
	if (status != sizeof(ch)) {
		ErrPrint("write: %d\n", errno);
		/* Forcely terminate the thread */
		status = pthread_cancel(s_info.push_thid);
		if (status != 0) {
			ErrPrint("cancel: %d\n", status);
		}
	}

	status = pthread_join(s_info.push_thid, &retval);
	if (status != 0) {
		ErrPrint("join: %d\n", status);
	}

	CRITICAL_SECTION_BEGIN(&s_info.request_list_lock);
	EINA_LIST_FREE(s_info.request_list, item) {
		destroy_request_item(item);
	}
	CRITICAL_SECTION_END(&s_info.request_list_lock);

	service_common_destroy(s_info.svc_ctx);
	s_info.svc_ctx = NULL;

	status = pthread_mutex_destroy(&s_info.request_list_lock);
	if (status != 0) {
		ErrPrint("destroy mutex: %d\n", status);
	}

	CLOSE_PIPE(s_info.request_pipe);

	DbgPrint("Successfully Finalized\n");
	return WIDGET_ERROR_NONE;
}

/* End of a file */
