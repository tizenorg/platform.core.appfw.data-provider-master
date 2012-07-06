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

int errno;

struct buffer {
	enum {
		CREATED = 0x00beef00,
		DESTROYED = 0x00dead00,
	} state;
	enum {
		BUFFER_FILE = 0x0,
		BUFFER_SHM = 0x1,
	} type;
	int refcnt;
	char data[];
};

struct fb_info {
	enum fb_type type;

	Ecore_Evas *ee;
	int w;
	int h;
	char *filename;

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

		info->buffer->type = BUFFER_FILE;

		snprintf(info->filename, fname_len, "%s%lf", g_conf.path.image, util_timestamp());
	} else if (info->type == FB_TYPE_SHM) {
		int id;

		id = shmget(IPC_PRIVATE, size + sizeof(*info->buffer), IPC_CREAT | 0644);
		if (id < 0) {
			ErrPrint("shmget: %s\n", strerror(errno));
			return NULL;
		}

		info->buffer = shmat(id, NULL, 0);
		if (!info->buffer) {
			ErrPrint("%s shmat: %s\n", info->filename, strerror(errno));

			if (shmctl(id, IPC_RMID, 0) < 0)
				ErrPrint("%s shmctl: %s\n", info->filename, strerror(errno));

			return NULL;
		}

		info->buffer->type = BUFFER_SHM;

		snprintf(info->filename, fname_len, "shm://%d", id);
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
		free(info->buffer);
		info->buffer = NULL;

		strncpy(info->filename, "undefined", fname_len);
	} else if (info->type == FB_TYPE_SHM) {
		int id;

		if (sscanf(info->filename, "shm://%d", &id) != 1) {
			ErrPrint("Unable to get the SHMID\n");
			return;
		}

		if (shmdt(info->buffer) < 0)
			ErrPrint("Failed to detatch: %s\n", strerror(errno));

		info->buffer = NULL;

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("%s shmctl: %s\n", info->filename, strerror(errno));

		strncpy(info->filename, "undefined", fname_len);
	}
}

struct fb_info *fb_create(int w, int h, enum fb_type type)
{
	struct fb_info *info;
	int fname_len;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	fname_len = strlen(g_conf.path.image) + 30;

	info->w = w;
	info->h = h;
	info->type = type;
	info->filename = malloc(fname_len);
	if (!info->filename) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(info);
		return NULL;
	}
	info->buffer = NULL;
	info->ee = NULL;

	strncpy(info->filename, "undefined", fname_len);
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

	free(info->filename);
	free(info);
	return 0;
}

Ecore_Evas * const fb_canvas(struct fb_info *info)
{
	return info->ee;
}

const char *fb_filename(struct fb_info *fb)
{
	if (!fb)
		return "";

	return fb->filename;
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

	fd = open(info->filename, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		ErrPrint("%s open falied: %s\n", info->filename, strerror(errno));
		return;
	}

	if (write(fd, info->buffer, info->bufsz) != info->bufsz) {
		ErrPrint("Write is not completed: %s\n", strerror(errno));
		close(fd);
		return;
	}

	close(fd);
}

/* End of a file */
