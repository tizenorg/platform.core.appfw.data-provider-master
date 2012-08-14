#include <stdio.h>
#include <unistd.h> /* access */
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include <dlog.h>

#include <Ecore_Evas.h>

#include "util.h"
#include "conf.h"
#include "debug.h"
#include "fb.h"
#include "buffer_handler.h"

int errno;

struct buffer {
	enum {
		CREATED = 0x00beef00,
		DESTROYED = 0x00dead00,
	} state;
	enum fb_type type;
	int refcnt;
	char data[];
};

struct fb_info {
	enum fb_type type;

	Ecore_Evas *ee;
	int w;
	int h;
	char *id;

	struct buffer *buffer;
	int bufsz;
};

int fb_init(void)
{
	return 0;
}

int fb_fini(void)
{
	return 0;
}

static void *alloc_fb(void *data, int size)
{
	struct fb_info *info;
	int fname_len;

	info = data;

	if (size != info->w * info->h * sizeof(int))
		ErrPrint("Buffer size is not matched (%d <> %d)\n", size, info->w * info->h * sizeof(int));

	fname_len = strlen(g_conf.path.image) + 30;

	if (info->type == FB_TYPE_FILE) {
		info->buffer = calloc(1, size + sizeof(*info->buffer));
		if (!info->buffer) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return NULL;
		}

		info->buffer->type = BUFFER_TYPE_FILE;
		info->buffer->refcnt = 0;
		info->buffer->state = CREATED; /*!< needless */

		snprintf(info->id, fname_len, "file://%s%lf", g_conf.path.image, util_timestamp());
	} else if (info->type == FB_TYPE_SHM) {
		int id;

		id = shmget(IPC_PRIVATE, size + sizeof(*info->buffer), IPC_CREAT | 0666);
		if (id < 0) {
			ErrPrint("shmget: %s\n", strerror(errno));
			return NULL;
		}

		info->buffer = shmat(id, NULL, 0);
		if (info->buffer == (void *)-1) {
			ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

			if (shmctl(id, IPC_RMID, 0) < 0)
				ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

			return NULL;
		}

		/*!
		 * \note
		 * Initiate the buffer control information
		 */
		info->buffer->type = BUFFER_TYPE_SHM;
		info->buffer->refcnt = id;
		info->buffer->state = CREATED; /* Meaniningless */

		snprintf(info->id, fname_len, "shm://%d", id);
	} else if (info->type == FB_TYPE_PIXMAP) {
		ErrPrint("Pixmap is not supported yet\n");
		strncpy(info->id, fname_len, "pixmap://-1");
	}

	info->bufsz = size;
	return info->buffer->data;
}

static void free_fb(void *data, void *ptr)
{
	struct fb_info *info;
	int fname_len;

	info = data;

	if (!info->buffer) {
		ErrPrint("Buffer is not valid (maybe already released)\n");
		return;
	}

	if (info->buffer->data != ptr)
		ErrPrint("Buffer pointer is not matched\n");

	fname_len = strlen(g_conf.path.image) + 30;

	if (info->type == FB_TYPE_FILE) {
		unlink(URI_TO_PATH(info->id));
		free(info->buffer);
		info->buffer = NULL;

		strncpy(info->id, "file:///tmp/.live.undefined", fname_len);
	} else if (info->type == FB_TYPE_SHM) {
		int id;

		if (sscanf(info->id, "shm://%d", &id) != 1) {
			ErrPrint("Unable to get the SHMID\n");
			return;
		}

		if (shmdt(info->buffer) < 0)
			ErrPrint("Failed to detatch: %s\n", strerror(errno));

		info->buffer = NULL;

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

		strncpy(info->id, "shm://-1", fname_len);
	} else if (info->type == FB_TYPE_PIXMAP) {
		ErrPrint("Pixmap is not supported yet\n");
		strncpy(info->id, "pixmap://-1", fname_len);
	}
}

struct fb_info *fb_create(int w, int h, enum fb_type type)
{
	struct fb_info *info;
	int fname_len;

	if (type != FB_TYPE_FILE && type != FB_TYPE_SHM) {
		ErrPrint("Invalid type\n");
		return NULL;
	}

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	fname_len = strlen(g_conf.path.image) + 30;

	info->w = w;
	info->h = h;
	info->type = type;
	info->id = malloc(fname_len);
	if (!info->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(info);
		return NULL;
	}
	info->buffer = NULL;
	info->ee = NULL;

	if (type == FB_TYPE_FILE)
		strncpy(info->id, "file:///tmp/.live.undefined", fname_len);
	else if (type == FB_TYPE_SHM)
		strncpy(info->id, "shm://-1", fname_len);
	else if (type == FB_TYPE_PIXMAP)
		strncpy(info->id, "pixmap://-1", fname_len);

	DbgPrint("FB Created [%dx%d]\n", info->w, info->h);
	return info;
}

int fb_create_buffer(struct fb_info *info)
{
	if (info->ee) {
		int w = 0;
		int h = 0;

		ecore_evas_geometry_get(info->ee, NULL, NULL, &w, &h);
		if (w != info->w || h != info->h) {
			ErrPrint("EE exists, size mismatched requested (%dx%d) but (%dx%d)\n", info->w, info->h, w, h);
			ecore_evas_resize(info->ee, info->w, info->h);
		}

		return 0;
	}

	info->ee = ecore_evas_buffer_allocfunc_new(info->w, info->h, alloc_fb, free_fb, info);
	if (!info->ee) {
		ErrPrint("Failed to create a buffer\n");
		return -EFAULT;
	}

	ecore_evas_alpha_set(info->ee, EINA_TRUE);
	ecore_evas_manual_render_set(info->ee, EINA_FALSE);
	ecore_evas_resize(info->ee, info->w, info->h);
	return 0;
}

int fb_destroy_buffer(struct fb_info *info)
{
	if (!info->ee) {
		ErrPrint("EE is not exists\n");
		return -EINVAL;
	}

	ecore_evas_free(info->ee);
	info->ee = NULL;
	return 0;
}

int fb_destroy(struct fb_info *info)
{
	if (info->ee) {
		ErrPrint("EE is not destroyed\n");
		return -EINVAL;
	}

	free(info->id);
	free(info);
	return 0;
}

Ecore_Evas * const fb_canvas(struct fb_info *info)
{
	return info->ee;
}

const char *fb_id(struct fb_info *fb)
{
	return fb ? fb->id : "";
}

int fb_resize(struct fb_info *info, int w, int h)
{
	info->w = w;
	info->h = h;

	if (info->ee)
		ecore_evas_resize(info->ee, info->w, info->h);

	return 0;
}

void fb_get_size(struct fb_info *info, int *w, int *h)
{
	*w = info->w;
	*h = info->h;
}

void fb_sync(struct fb_info *info)
{
	int fd;
	if (!info->buffer || info->type != FB_TYPE_FILE)
		return;

	fd = open(URI_TO_PATH(info->id), O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		ErrPrint("%s open falied: %s\n", URI_TO_PATH(info->id), strerror(errno));
		return;
	}

	if (write(fd, info->buffer, info->bufsz) != info->bufsz) {
		ErrPrint("Write is not completed: %s\n", strerror(errno));
		close(fd);
		return;
	}

	close(fd);
}

enum fb_type fb_type(struct fb_info *info)
{
	return info->type;
}

/* End of a file */
