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
#include "client_life.h"
#include "instance.h"
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

	struct inst_info *inst;
};

struct buffer_info *buffer_handler_create(struct inst_info *inst, enum buffer_type type, int w, int h, int pixel_size)
{
	struct buffer_info *info;

	info = malloc(sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->id = strdup("file:///tmp/.live.undefined");
	if (!info->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(info);
		return NULL;
	}

	info->w = w;
	info->h = h;
	info->pixel_size = pixel_size;
	info->type = type;
	info->inst = inst;
	info->is_loaded = 0;

	return info;
}

int buffer_handler_load(struct buffer_info *info)
{
	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (info->is_loaded) {
		DbgPrint("Buffer is already loaded\n");
		return 0;
	}

	if (info->type == BUFFER_TYPE_FILE) {
		int len;
		double timestamp;

		len = strlen(g_conf.path.image) + 40;
		timestamp = util_timestamp();

		free(info->id);
		info->id = malloc(len);
		snprintf(info->id, len, "file://%s%lf", g_conf.path.image, timestamp);
	} else if (info->type == BUFFER_TYPE_SHM) {
		int id;
		int size;
		int len;
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
		if (!info->buffer) {
			ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

			if (shmctl(id, IPC_RMID, 0) < 0)
				ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

			return -EFAULT;
		}

		free(info->id);

		len = 6 + 30; /* strlen("shm://") + 30 */
		info->id = malloc(len);
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			shmdt(info->buffer);
			shmctl(id, IPC_RMID, 0);
			info->buffer = NULL;
			return -ENOMEM;
		}

		snprintf(info->id, len, "shm://%d", id);
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
		int len;
		char *path;

		len = strlen(info->id);
		path = malloc(len);
		if (!path) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return -ENOMEM;
		}

		if (sscanf(info->id, "file://%s", path) != 1) {
			free(path);
			ErrPrint("Invalid argument\n");
			return -EINVAL;
		}

		if (unlink(path) < 0)
			ErrPrint("unlink: %s\n", strerror(errno));

		free(info->id);
		info->id = strdup("");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
	} else if (info->type == BUFFER_TYPE_SHM) {
		int id;

		if (sscanf(info->id, "ssh://%d", &id) != 1) {
			ErrPrint("Invalid ID\n");
			return -EINVAL;
		}

		shmdt(info->buffer);
		shmctl(id, IPC_RMID, 0);
		info->buffer = NULL;
		free(info->id);
		info->id = strdup("");
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

	info->w = w;
	info->h = h;

	if (!info->is_loaded) {
		DbgPrint("Not yet loaded, just update the size [%dx%d]\n", w, h);
		return 0;
	}

	if (info->type != BUFFER_TYPE_SHM)
		return 0;

	if (sscanf(info->id, "shm://%d", &id) != 1) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	if (shmdt(info->buffer) < 0) {
		ErrPrint("detach failed [%s]\n", strerror(errno));
		return -EINVAL;
	}

	if (shmctl(id, IPC_RMID, 0) < 0) {
		ErrPrint("Ctrl failed [%s]\n", strerror(errno));
		return -EINVAL;
	}

	info->buffer = NULL;

	free(info->id);
	info->id = NULL;

	size = info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Invalid buffer size\n");
		info->id = strdup("shm://-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
		return -EINVAL;
	}

	id = shmget(IPC_PRIVATE, size + sizeof(*buffer), IPC_CREAT | 0666);
	if (id < 0) {
		ErrPrint("shmget: %s\n", strerror(errno));
		info->id = strdup("shm://-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
		return -EFAULT;
	}

	info->buffer = shmat(id, NULL, 0);
	if (!info->buffer) {
		ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

		info->id = strdup("shm://-1");
		if (!info->id)
			ErrPrint("Heap: %s\n", strerror(errno));
		return -EFAULT;
	}

	len = 6 + 30; /* strlen("shm://") + 30 */
	info->id = malloc(len);
	if (!info->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		shmdt(info->buffer);
		shmctl(id, IPC_RMID, 0);
		info->buffer = NULL;
		return -ENOMEM;
	}

	snprintf(info->id, len, "shm://%d", id);
	return 0;
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
