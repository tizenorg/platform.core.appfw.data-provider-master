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
#include "buffer_handler.h"
#include "fb.h"

int errno;

struct fb_info {
	Ecore_Evas *ee;

	struct buffer_info *buffer;
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
	struct fb_info *info = data;

	DbgPrint("FB size: %d\n", size);

	if (buffer_handler_load(info->buffer) < 0) {
		ErrPrint("Failed to load buffer handler\n");
		return NULL;
	}

	return buffer_handler_buffer(info->buffer);
}

static void free_fb(void *data, void *ptr)
{
	struct fb_info *info = data;

	if (!info->buffer) {
		ErrPrint("Buffer is not valid (maybe already released)\n");
		return;
	}

	if (buffer_handler_buffer(info->buffer) != ptr)
		ErrPrint("Buffer pointer is not matched\n");

	(void)buffer_handler_unload(info->buffer);
}

struct fb_info *fb_create(int w, int h, enum buffer_type type)
{
	struct fb_info *info;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->buffer = buffer_handler_create(type, w, h, sizeof(int));
	if (!info->buffer) {
		ErrPrint("Failed to create a buffer\n");
		free(info);
		return NULL;
	}

	info->ee = NULL;

	return info;
}

int fb_create_buffer(struct fb_info *info)
{
	int ow;
	int oh;

	buffer_handler_get_size(info->buffer, &ow, &oh);

	if (info->ee) {
		int w = 0;
		int h = 0;

		ecore_evas_geometry_get(info->ee, NULL, NULL, &w, &h);
		if (w != ow || h != oh) {
			ErrPrint("EE exists, size mismatched requested (%dx%d) but (%dx%d)\n", ow, oh, w, h);
			ecore_evas_resize(info->ee, ow, oh);
		}

		return 0;
	}

	info->ee = ecore_evas_buffer_allocfunc_new(ow, oh, alloc_fb, free_fb, info);
	if (!info->ee) {
		ErrPrint("Failed to create a buffer\n");
		return -EFAULT;
	}

	ecore_evas_alpha_set(info->ee, EINA_TRUE);
	ecore_evas_manual_render_set(info->ee, EINA_FALSE);
	ecore_evas_resize(info->ee, ow, oh);
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
	fb_destroy_buffer(info);
	free(info);
	return 0;
}

Ecore_Evas * const fb_canvas(struct fb_info *info)
{
	return info->ee;
}

const char *fb_id(struct fb_info *fb)
{
	return fb ? buffer_handler_id(fb->buffer) : "";
}

int fb_resize(struct fb_info *info, int w, int h)
{
	if (info->ee) /* This will do free/alloc operation */
		ecore_evas_resize(info->ee, w, h);
	else /* Just update the buffer information */
		buffer_handler_resize(info->buffer, w, h);

	return 0;
}

void fb_get_size(struct fb_info *info, int *w, int *h)
{
	buffer_handler_get_size(info->buffer, w, h);
}

void fb_sync(struct fb_info *info)
{
	buffer_handler_flush(info->buffer);
}

/* End of a file */
