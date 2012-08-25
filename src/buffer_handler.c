#include <stdio.h>
#include <unistd.h> /* access */
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <string.h>
#include <stdlib.h>

#include <dlog.h>
#include <packet.h>

#include "debug.h"
#include "conf.h"
#include "util.h"
#include "instance.h"
#include "client_life.h"
#include "client_rpc.h"
#include "buffer_handler.h"

struct buffer {
	enum {
		CREATED = 0x00beef00,
		DESTROYED = 0x00dead00,
	} state;
	enum buffer_type type;
	int refcnt;
	char data[];
};

struct buffer_info
{
	struct buffer *buffer;
	char *id;

	enum buffer_type type;

	int w;
	int h;
	int pixel_size;
	int is_loaded;
};

struct buffer_info *buffer_handler_create(enum buffer_type type, int w, int h, int pixel_size)
{
	struct buffer_info *info;

	info = malloc(sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	if (type == BUFFER_TYPE_SHM) {
		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(info);
			return NULL;
		}
	} else if (type == BUFFER_TYPE_FILE) {
		info->id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(info);
			return NULL;
		}
	} else if (type == BUFFER_TYPE_PIXMAP) {
		info->id = strdup(SCHEMA_PIXMAP "-1");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			free(info);
			return NULL;
		}
	} else {
		ErrPrint("Invalid type\n");
		free(info);
		return NULL;
	}

	info->w = w;
	info->h = h;
	info->pixel_size = pixel_size;
	info->type = type;
	info->is_loaded = 0;

	return info;
}

int buffer_handler_load(struct buffer_info *info)
{
	int len;

	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (info->is_loaded) {
		DbgPrint("Buffer is already loaded\n");
		return 0;
	}

	if (info->type == BUFFER_TYPE_FILE) {
		double timestamp;
		int size;
		struct buffer *buffer;

		size = sizeof(*buffer) + info->w * info->h * info->pixel_size;
		info->buffer = calloc(1, size);
		if (!info) {
			ErrPrint("Failed to allocate buffer\n");
			return -ENOMEM;
		}

		info->buffer->type = BUFFER_TYPE_FILE;
		info->buffer->refcnt = 0;
		info->buffer->state = CREATED;

		len = strlen(g_conf.path.image) + 40;
		timestamp = util_timestamp();

		free(info->id);

		info->id = malloc(len);
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return -ENOMEM;
		}

		snprintf(info->id, len, SCHEMA_FILE "%s%lf", g_conf.path.image, timestamp);
		DbgPrint("FILE type %d created\n", size);
	} else if (info->type == BUFFER_TYPE_SHM) {
		int id;
		int size;
		struct buffer *buffer; /* Just for getting a size */

		size = info->w * info->h * info->pixel_size;
		if (!size) {
			ErrPrint("Invalid buffer size\n");
			return -EINVAL;
		}

		id = shmget(IPC_PRIVATE, size + sizeof(*buffer), IPC_CREAT | 0666);
		if (id < 0) {
			ErrPrint("shmget: %s\n", strerror(errno));
			return -EFAULT;
		}

		info->buffer = shmat(id, NULL, 0);
		if (info->buffer == (void *)-1) {
			ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

			if (shmctl(id, IPC_RMID, 0) < 0)
				ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

			return -EFAULT;
		}

		info->buffer->type = BUFFER_TYPE_SHM;
		info->buffer->refcnt = id;
		info->buffer->state = CREATED; /*!< Needless */

		free(info->id);

		len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */
		info->id = malloc(len);
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			shmdt(info->buffer);
			shmctl(id, IPC_RMID, 0);
			info->buffer = NULL;
			return -ENOMEM;
		}

		snprintf(info->id, len, SCHEMA_SHM "%d", id);
	} else if (info->type == BUFFER_TYPE_PIXMAP) {
		/*
		 */
		free(info->id);

		len = strlen(SCHEMA_PIXMAP) + 30; /* strlen("pixmap://") + 30 */
		info->id = malloc(len);
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return -ENOMEM;
		}

		strncpy(info->id, SCHEMA_PIXMAP "-1", len);
	} else {
		ErrPrint("Invalid buffer\n");
		return -EINVAL;
	}

	info->is_loaded = 1;
	return 0;
}

int buffer_handler_unload(struct buffer_info *info)
{
	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return -EINVAL;
	}

	if (info->type == BUFFER_TYPE_FILE) {
		const char *path;

		free(info->buffer);
		info->buffer = NULL;

		path = util_uri_to_path(info->id);
		if (path && unlink(path) < 0)
			ErrPrint("unlink: %s\n", strerror(errno));

		free(info->id);

		info->id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));

	} else if (info->type == BUFFER_TYPE_SHM) {
		int id;

		if (sscanf(info->id, SCHEMA_SHM "%d", &id) != 1) {
			ErrPrint("Invalid ID\n");
			return -EINVAL;
		}

		if (info->id < 0) {
			ErrPrint("Invalid id\n");
			return -EINVAL;
		}

		if (shmdt(info->buffer) < 0)
			ErrPrint("Detach shm: %s\n", strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("Remove shm: %s\n", strerror(errno));

		info->buffer = NULL;

		free(info->id);

		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
	} else if (info->type == BUFFER_TYPE_PIXMAP) {
		free(info->id);
		info->id = strdup(SCHEMA_PIXMAP "-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
	} else {
		ErrPrint("Invalid buffer\n");
		return -EINVAL;
	}

	info->is_loaded = 0;
	return 0;
}

int buffer_handler_destroy(struct buffer_info *info)
{
	if (info->type == BUFFER_TYPE_SHM) {
		if (info->buffer) {
			ErrPrint("BUFFER is still loaded\n");
			buffer_handler_unload(info);
		}
	} else if (info->type == BUFFER_TYPE_FILE) {
		unlink(info->id);
	} else if (info->type == BUFFER_TYPE_PIXMAP) {
		ErrPrint("Pixmap is not supported yet\n");
	}

	free(info->id);
	free(info);
	return 0;
}

const char *buffer_handler_id(const struct buffer_info *info)
{
	return info ? info->id : "";
}

enum buffer_type buffer_handler_type(const struct buffer_info *info)
{
	return info ? info->type : BUFFER_TYPE_ERROR;
}

void *buffer_handler_fb(const struct buffer_info *info)
{
	return info ? info->buffer->data : NULL;
}

int buffer_handler_is_loaded(const struct buffer_info *info)
{
	return info ? info->is_loaded : 0;
}

void buffer_handler_update_size(struct buffer_info *info, int w, int h)
{
	if (!info)
		return;

	info->w = w;
	info->h = h;
}

int buffer_handler_resize(struct buffer_info *info, int w, int h)
{
	int id;
	int size;
	int len;
	struct buffer *buffer;

	if (!info) {
		ErrPrint("Invalid handler\n");
		return -EINVAL;
	}

	if (info->w == w && info->h == h) {
		DbgPrint("No changes\n");
		return 0;
	}

	buffer_handler_update_size(info, w, h);

	if (!info->is_loaded) {
		DbgPrint("Not yet loaded, just update the size [%dx%d]\n", w, h);
		return 0;
	}

	if (info->type != BUFFER_TYPE_SHM)
		return 0;

	if (sscanf(info->id, SCHEMA_SHM "%d", &id) != 1) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	if (shmdt(info->buffer) < 0) {
		ErrPrint("shmdt: [%s]\n", strerror(errno));
		return -EINVAL;
	}

	if (shmctl(id, IPC_RMID, 0) < 0) {
		ErrPrint("shmctl: [%s]\n", strerror(errno));
		return -EINVAL;
	}

	info->buffer = NULL;

	free(info->id);
	info->id = NULL;

	size = info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Invalid buffer size\n");
		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
		return -EINVAL;
	}

	id = shmget(IPC_PRIVATE, size + sizeof(*buffer), IPC_CREAT | 0666);
	if (id < 0) {
		ErrPrint("shmget: %s\n", strerror(errno));
		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
		return -EFAULT;
	}

	info->buffer = shmat(id, NULL, 0);
	if (info->buffer == (void *)-1) {
		ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
		return -EFAULT;
	}

	/*!
	 * refcnt is used for keeping the IPC resource ID
	 */
	info->buffer->refcnt = id;
	info->buffer->state = CREATED;
	info->buffer->type = BUFFER_TYPE_SHM;

	len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */
	info->id = malloc(len);
	if (!info->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		shmdt(info->buffer);
		shmctl(id, IPC_RMID, 0);
		info->buffer = NULL;
		return -ENOMEM;
	}

	snprintf(info->id, len, SCHEMA_SHM "%d", id);
	return 0;
}

int buffer_handler_get_size(struct buffer_info *info, int *w, int *h)
{
	if (!info)
		return -EINVAL;

	if (w)
		*w = info->w;
	if (h)
		*h = info->h;

	return 0;
}

void buffer_handler_flush(struct buffer_info *info)
{
	int fd;
	int size;

	if (!info || !info->buffer || info->type != BUFFER_TYPE_FILE)
		return;

	fd = open(util_uri_to_path(info->id), O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		ErrPrint("%s open falied: %s\n", util_uri_to_path(info->id), strerror(errno));
		return;
	}

	size = info->w * info->h * info->pixel_size;
	DbgPrint("Flush size: %d\n", size);

	if (write(fd, info->buffer, size) != size)
		ErrPrint("Write is not completed: %s\n", strerror(errno));

	close(fd);
}

int buffer_handler_init(void)
{
	return 0;
}

int buffer_handler_fini(void)
{
	return 0;
}

/* End of a file */
