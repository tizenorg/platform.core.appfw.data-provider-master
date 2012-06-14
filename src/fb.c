#include <stdio.h>
#include <unistd.h> /* access */
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <dlog.h>

#include <Ecore_Evas.h>

#include "util.h"
#include "conf.h"
#include "debug.h"
#include "fb.h"

int errno;

struct fb_info {
	Ecore_Evas *ee;
	int w;
	int h;
	char *filename;
	int fd;

	void *buffer;
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

	info = data;

	info->fd = open(info->filename, O_RDWR | O_CREAT, 0644);
	if (info->fd < 0) {
		ErrPrint("%s open failed: %s\n", info->filename, strerror(errno));
		if (unlink(info->filename) < 0)
			ErrPrint("unlink: %s - %s\n", info->filename, strerror(errno));
		return NULL;
	}

	info->buffer = calloc(1, size);
	if (!info->buffer) {
		close(info->fd);
		info->fd = -EINVAL;
		if (unlink(info->filename) < 0)
			ErrPrint("unlink: %s - %s\n", info->filename, strerror(errno));

		return NULL;
	}

	info->bufsz = size;
	return info->buffer;
}

static void free_fb(void *data, void *ptr)
{
	struct fb_info *info;

	info = data;

//	munmap(info->buffer, info->bufsz);
	if (info->buffer) {
		free(info->buffer);
		info->buffer = NULL;
	}

	if (info->fd >= 0) {
		close(info->fd);
		info->fd = -EINVAL;
	}

	if (unlink(info->filename) < 0)
		ErrPrint("Unlink: %s - %s\n", info->filename, strerror(errno));
}

struct fb_info *fb_create(const char *filename, int w, int h)
{
	struct fb_info *info;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->w = w;
	info->h = h;

	info->filename = strdup(filename);
	if (!info->filename) {
		ErrPrint("Heap: %s\n", strerror(errno));
		free(info);
		return NULL;
	}

	info->fd = -EINVAL;
	info->ee = NULL;

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
	return (fb && fb->filename) ? fb->filename : "";
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

/*
static inline struct flock *file_lock(short type, short whence)
{
	static struct flock ret;

	ret.l_type = type;
	ret.l_start = 0;
	ret.l_whence = whence;
	ret.l_len = 0;
	ret.l_pid = getpid();
	return &ret;
}
*/

void fb_sync(struct fb_info *info)
{
	if (info->fd < 0 || !info->buffer)
		return;

//	fcntl(info->fd, F_SETLKW, file_lock(F_WRLCK, SEEK_SET));

	if (lseek(info->fd, 0l, SEEK_SET) != 0) {
		ErrPrint("Failed to do seek : %s\n", strerror(errno));
//		fcntl(info->fd, F_SETLKW, file_lock(F_UNLCK, SEEK_SET));
		return;
	}

	if (write(info->fd, info->buffer, info->bufsz) != info->bufsz)
		ErrPrint("Write is not completed: %s\n", strerror(errno));

//	fcntl(info->fd, F_SETLKW, file_lock(F_UNLCK, SEEK_SET));
//	if (msync(info->buffer, info->bufsz, MS_SYNC) < 0)
//		ErrPrint("Sync: %s\n", strerror(errno));
}

/* End of a file */
