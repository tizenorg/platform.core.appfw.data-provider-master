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

#include <Ecore.h>
#include <Ecore_Wayland.h> /* wayland_client.h is included by this header */

#include <tbm_bufmgr.h>

#include <dlog.h>
#include <packet.h>
#include <widget_errno.h>
#include <widget_util.h>
#include <widget_conf.h>
#include <widget_buffer.h>
#include <widget_service.h>
#include <widget_service_internal.h>

#include "debug.h"
#include "conf.h"
#include "util.h"
#include "instance.h"
#include "package.h"
#include "client_life.h"
#include "client_rpc.h"
#include "buffer_handler.h"
#include "script_handler.h" // Reverse dependency. must has to be broken

struct gem_data {
	tbm_bo pixmap_bo;
	int count;
	int buf_count;
	int w;
	int h;
	int depth;
	void *data; /* Gem layer */
	int refcnt;
	int pixmap; /* FD in case of wayland */
};

struct buffer_info
{
	void *buffer;
	char *id;
	widget_lock_info_t lock_info;

	enum widget_fb_type type;

	int w;
	int h;
	int pixel_size;
	int is_loaded;

	struct inst_info *inst;
	void *data;
};

static struct {
	tbm_bufmgr slp_bufmgr;
	int fd;
	Eina_List *pixmap_list;
} s_info = {
	.slp_bufmgr = NULL,
	.fd = -1,
	.pixmap_list = NULL,
};


static inline widget_fb_t create_pixmap(struct buffer_info *info)
{
	struct gem_data *gem;
	widget_fb_t buffer;

	buffer = calloc(1, sizeof(*buffer) + sizeof(*gem));
	if (!buffer) {
		ErrPrint("calloc: %d\n", errno);
		return NULL;
	}

	gem = (struct gem_data *)buffer->data;

	buffer->type = WIDGET_FB_TYPE_PIXMAP;
	buffer->refcnt = 1;
	buffer->state = WIDGET_FB_STATE_CREATED;

	gem->count = 1;
	gem->w = info->w;
	gem->h = info->h;
	gem->depth = WIDGET_CONF_DEFAULT_PIXELS;

	return buffer;
}

static inline void *acquire_gem(widget_fb_t buffer)
{
	struct gem_data *gem;

	if (!buffer) {
		return NULL;
	}

	gem = (struct gem_data *)buffer->data;
	if (s_info.fd < 0) {
		ErrPrint("GEM is not supported - Use the fake gem buffer\n");
	} else {
		if (!gem->pixmap_bo) {
			ErrPrint("GEM is not created\n");
			return NULL;
		}

		if (!gem->data) {
			tbm_bo_handle handle;

			if (gem->refcnt) {
				ErrPrint("Already acquired. but the buffer is not valid\n");
				return NULL;
			}

			handle = tbm_bo_map(gem->pixmap_bo, TBM_DEVICE_CPU, TBM_OPTION_READ | TBM_OPTION_WRITE);
			gem->data = handle.ptr;
		}
	}

	gem->refcnt++;
	return gem->data;
}

static inline void release_gem(widget_fb_t buffer)
{
	struct gem_data *gem;

	gem = (struct gem_data *)buffer->data;
	if (s_info.fd >= 0 && !gem->pixmap_bo) {
		ErrPrint("GEM is not created\n");
		return;
	}

	if (!gem->data) {
		if (gem->refcnt > 0) {
			ErrPrint("Reference count is not valid %d\n", gem->refcnt);
			gem->refcnt = 0;
		}
		return;
	}

	gem->refcnt--;
	if (gem->refcnt == 0) {
		if (s_info.fd < 0) {
			DbgPrint("S/W Gem buffer has no reference\n");
		} else {
			if (gem->pixmap_bo) {
				tbm_bo_unmap(gem->pixmap_bo);
				gem->pixmap_bo = NULL;
			}

			gem->data = NULL;
		}
	} else if (gem->refcnt < 0) {
		ErrPrint("Invalid refcnt: %d (reset)\n", gem->refcnt);
		gem->refcnt = 0;
	}
}


static inline int destroy_pixmap(widget_fb_t buffer)
{
	/**
	 * @note
	 * gem data should be destroyed first.
	 */
	buffer->state = WIDGET_FB_STATE_DESTROYED;
	DbgFree(buffer);
	return WIDGET_ERROR_NONE;
}

static inline int create_gem(widget_fb_t buffer)
{
	struct gem_data *gem;

	gem = (struct gem_data *)buffer->data;

	gem->pixmap_bo = tbm_bo_alloc(s_info.slp_bufmgr, gem->w * gem->h * gem->depth, TBM_BO_DEFAULT);
	if (!gem->pixmap_bo) {
		ErrPrint("Failed to create Buffer Object\n");
		return WIDGET_ERROR_FAULT;
	}

	gem->pixmap = tbm_bo_export(gem->pixmap_bo);
	if (gem->pixmap < 0) {
		ErrPrint("Failed to export FD\n");
		tbm_bo_unref(gem->pixmap_bo);
		gem->pixmap_bo = NULL;
		return WIDGET_ERROR_FAULT;
	}

	return WIDGET_ERROR_NONE;
}

static inline int destroy_gem(widget_fb_t buffer)
{
	struct gem_data *gem;

	if (!buffer) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	/*!
	 * Forcely release the acquire_buffer.
	 */
	gem = (struct gem_data *)buffer->data;
	if (!gem) {
		return WIDGET_ERROR_FAULT;
	}

	if (s_info.fd >= 0) {
		if (gem->pixmap_bo) {
			DbgPrint("unref pixmap bo\n");
			tbm_bo_unref(gem->pixmap_bo);
			gem->pixmap_bo = NULL;
			gem->pixmap = 0;
		}
	} else if (gem->data) {
		DbgPrint("Release fake gem buffer\n");
		DbgFree(gem->data);
		gem->data = NULL;
	}

	return WIDGET_ERROR_NONE;
}

static inline int load_file_buffer(struct buffer_info *info)
{
	widget_fb_t buffer;
	double timestamp;
	int size;
	char *new_id;
	int len;

	len = strlen(WIDGET_CONF_IMAGE_PATH) + 40;
	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	timestamp = util_timestamp();
	snprintf(new_id, len, SCHEMA_FILE "%s%lf", WIDGET_CONF_IMAGE_PATH, timestamp);

	size = sizeof(*buffer) + info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Canvas buffer size is ZERO\n");
		DbgFree(new_id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	buffer = calloc(1, size);
	if (!buffer) {
		ErrPrint("Failed to allocate buffer\n");
		DbgFree(new_id);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	buffer->type = WIDGET_FB_TYPE_FILE;
	buffer->refcnt = 0;
	buffer->state = WIDGET_FB_STATE_CREATED;
	buffer->info = info;

	DbgFree(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;

	DbgPrint("FILE type %d created\n", size);
	return WIDGET_ERROR_NONE;
}

static inline int load_shm_buffer(struct buffer_info *info)
{
	int id;
	int size;
	widget_fb_t buffer; /* Just for getting a size */
	char *new_id;
	int len;

	size = info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Invalid buffer size\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	id = shmget(IPC_PRIVATE, size + sizeof(*buffer), IPC_CREAT | 0666);
	if (id < 0) {
		ErrPrint("shmget: %d\n", errno);
		return WIDGET_ERROR_FAULT;
	}

	buffer = shmat(id, NULL, 0);
	if (buffer == (void *)-1) {
		ErrPrint("%s shmat: %d\n", info->id, errno);

		if (shmctl(id, IPC_RMID, 0) < 0) {
			ErrPrint("%s shmctl: %d\n", info->id, errno);
		}

		return WIDGET_ERROR_FAULT;
	}

	buffer->type = WIDGET_FB_TYPE_SHM;
	buffer->refcnt = id;
	buffer->state = WIDGET_FB_STATE_CREATED; /*!< Needless */
	buffer->info = (void *)((long)size); /*!< Use this field to indicates the size of SHM */

	len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */

	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("malloc: %d\n", errno);
		if (shmdt(buffer) < 0) {
			ErrPrint("shmdt: %d\n", errno);
		}

		if (shmctl(id, IPC_RMID, 0) < 0) {
			ErrPrint("shmctl: %d\n", errno);
		}

		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	snprintf(new_id, len, SCHEMA_SHM "%d", id);

	DbgFree(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;
	return WIDGET_ERROR_NONE;
}

static inline int load_pixmap_buffer(struct buffer_info *info)
{
	widget_fb_t buffer;
	struct gem_data *gem;
	char *new_id;
	int len;

	/*!
	 * \NOTE
	 * Before call the buffer_handler_pixmap_ref function,
	 * You should make sure that the is_loaded value is toggled (1)
	 * Or the buffer_handler_pixmap_ref function will return NULL
	 */
	info->is_loaded = 1;

	if (info->buffer) {
		DbgPrint("Buffer is already exists, but override it with new one\n");
	}

	buffer = buffer_handler_pixmap_ref(info);
	if (!buffer) {
		DbgPrint("Failed to make a reference of a pixmap\n");
		info->is_loaded = 0;
		return WIDGET_ERROR_FAULT;
	}

	len = strlen(SCHEMA_PIXMAP) + 30; /* strlen("pixmap://") + 30 */
	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("malloc: %d\n", errno);
		info->is_loaded = 0;
		buffer_handler_pixmap_unref(buffer);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->id);
	info->id = new_id;

	gem = (struct gem_data *)buffer->data;

	snprintf(info->id, len, SCHEMA_PIXMAP "%d:%d", (int)gem->pixmap, info->pixel_size);
	DbgPrint("Loaded pixmap(info->id): %s\n", info->id);
	return WIDGET_ERROR_NONE;
}

EAPI int buffer_handler_load(struct buffer_info *info)
{
	int ret;
	widget_target_type_e type = WIDGET_TYPE_GBAR;

	if (!info) {
		ErrPrint("buffer handler is nil\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (info->is_loaded) {
		DbgPrint("Buffer is already loaded\n");
		return WIDGET_ERROR_NONE;
	}

	switch (info->type) {
	case WIDGET_FB_TYPE_FILE:
		ret = load_file_buffer(info);
		if (script_handler_buffer_info(instance_gbar_script(info->inst)) != info && instance_gbar_buffer(info->inst) != info) {
			type = WIDGET_TYPE_WIDGET;
		}
		info->lock_info = widget_service_create_lock(instance_id(info->inst), type, WIDGET_LOCK_WRITE);
		break;
	case WIDGET_FB_TYPE_SHM:
		ret = load_shm_buffer(info);
		if (script_handler_buffer_info(instance_gbar_script(info->inst)) != info && instance_gbar_buffer(info->inst) != info) {
			type = WIDGET_TYPE_WIDGET;
		}
		info->lock_info = widget_service_create_lock(instance_id(info->inst), type, WIDGET_LOCK_WRITE);
		break;
	case WIDGET_FB_TYPE_PIXMAP:
		ret = load_pixmap_buffer(info);
		break;
	default:
		ErrPrint("Invalid buffer\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		break;
	}

	return ret;
}

static inline int unload_file_buffer(struct buffer_info *info)
{
	const char *path;
	char *new_id;

	new_id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
	if (!new_id) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->buffer);
	info->buffer = NULL;

	path = widget_util_uri_to_path(info->id);
	if (path && unlink(path) < 0) {
		ErrPrint("unlink: %s\n", errno);
	}

	DbgFree(info->id);
	info->id = new_id;
	return WIDGET_ERROR_NONE;
}

static inline int unload_shm_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;

	new_id = strdup(SCHEMA_SHM "-1");
	if (!new_id) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	if (sscanf(info->id, SCHEMA_SHM "%d", &id) != 1) {
		ErrPrint("%s Invalid ID\n", info->id);
		DbgFree(new_id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (id < 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		DbgFree(new_id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (shmdt(info->buffer) < 0) {
		ErrPrint("shmdt: %d\n", errno);
	}

	if (shmctl(id, IPC_RMID, 0) < 0) {
		ErrPrint("shmctl: %d\n", errno);
	}

	info->buffer = NULL;

	DbgFree(info->id);
	info->id = new_id;
	return WIDGET_ERROR_NONE;
}

static inline int unload_pixmap_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;
	int pixels;

	new_id = strdup(SCHEMA_PIXMAP "0:0");
	if (!new_id) {
		ErrPrint("strdup: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	if (sscanf(info->id, SCHEMA_PIXMAP "%d:%d", &id, &pixels) != 2) {
		ErrPrint("Invalid ID (%s)\n", info->id);
		DbgFree(new_id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (id == 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		DbgFree(new_id);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	/*!
	 * Decrease the reference counter.
	 */
	buffer_handler_pixmap_unref(info->buffer);

	/*!
	 * \note
	 * Just clear the info->buffer.
	 * It will be reallocated again.
	 */
	info->buffer = NULL;

	DbgFree(info->id);
	info->id = new_id;
	return WIDGET_ERROR_NONE;
}

EAPI int buffer_handler_unload(struct buffer_info *info)
{
	int ret;

	if (!info) {
		ErrPrint("buffer handler is NIL\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (info->type) {
	case WIDGET_FB_TYPE_FILE:
		widget_service_destroy_lock(info->lock_info);
		info->lock_info = NULL;
		ret = unload_file_buffer(info);
		break;
	case WIDGET_FB_TYPE_SHM:
		widget_service_destroy_lock(info->lock_info);
		info->lock_info = NULL;
		ret = unload_shm_buffer(info);
		break;
	case WIDGET_FB_TYPE_PIXMAP:
		ret = unload_pixmap_buffer(info);
		break;
	default:
		ErrPrint("Invalid buffer\n");
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		break;
	}

	if (ret == 0) {
		info->is_loaded = 0;
	}

	return ret;
}

EAPI const char *buffer_handler_id(const struct buffer_info *info)
{
	return info ? info->id : "";
}

EAPI enum widget_fb_type buffer_handler_type(const struct buffer_info *info)
{
	return info ? info->type : WIDGET_FB_TYPE_ERROR;
}

EAPI void *buffer_handler_fb(struct buffer_info *info)
{
	widget_fb_t buffer;

	if (!info) {
		return NULL;
	}

	buffer = info->buffer;

	if (info->type == WIDGET_FB_TYPE_PIXMAP) {
		void *canvas;
		int ret;

		/*!
		 * \note
		 * For getting the buffer address of gem.
		 */
		canvas = buffer_handler_pixmap_acquire_buffer(info);
		ret = buffer_handler_pixmap_release_buffer(canvas);
		if (ret < 0) {
			ErrPrint("Failed to release buffer: %d\n", ret);
		}
		return canvas;
	}

	return buffer->data;
}

EAPI int buffer_handler_pixmap(const struct buffer_info *info)
{
	widget_fb_t buf;
	struct gem_data *gem;

	if (!info) {
		ErrPrint("Inavlid buffer handler\n");
		return 0;
	}

	if (info->type != WIDGET_FB_TYPE_PIXMAP) {
		ErrPrint("Invalid buffer type\n");
		return 0;
	}

	buf = (widget_fb_t)info->buffer;
	if (!buf) {
		ErrPrint("Invalid buffer data\n");
		return 0;
	}

	gem = (struct gem_data *)buf->data;
	return gem->pixmap;
}

EAPI void *buffer_handler_pixmap_acquire_buffer(struct buffer_info *info)
{
	widget_fb_t buffer;

	if (!info || !info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	buffer = buffer_handler_pixmap_ref(info);
	if (!buffer) {
		return NULL;
	}

	return acquire_gem(buffer);
}

EAPI void *buffer_handler_pixmap_buffer(struct buffer_info *info)
{
	widget_fb_t buffer;
	struct gem_data *gem;

	if (!info) {
		return NULL;
	}

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	buffer = info->buffer;
	if (!buffer) {
		return NULL;
	}

	gem = (struct gem_data *)buffer->data;
	return gem->data;
}

/*!
 * \return "buffer" object (Not the buffer_info)
 */
EAPI void *buffer_handler_pixmap_ref(struct buffer_info *info)
{
	widget_fb_t buffer;

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	if (info->type != WIDGET_FB_TYPE_PIXMAP) {
		ErrPrint("Buffer type is not matched\n");
		return NULL;
	}

	buffer = info->buffer;
	if (!buffer) {
		int need_gem = 1;

		buffer = create_pixmap(info);
		if (!buffer) {
			ErrPrint("Failed to create a pixmap\n");
			return NULL;
		}

		info->buffer = buffer;

		if (info->inst) {
			struct pkg_info *pkg;

			pkg = instance_package(info->inst);

			if (instance_widget_buffer(info->inst) == info) {
				if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
					need_gem = 0;
				}
			} else if (instance_gbar_buffer(info->inst) == info) {
				if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
					need_gem = 0;
				}
			} else {
				int idx;

				for (idx = 0; idx < WIDGET_CONF_EXTRA_BUFFER_COUNT; idx++) {
					if (instance_widget_extra_buffer(info->inst, idx) == info) {
						if (package_widget_type(pkg) == WIDGET_TYPE_BUFFER) {
							need_gem = 0;
							break;
						}
					}

					if (instance_gbar_extra_buffer(info->inst, idx) == info) {
						if (package_gbar_type(pkg) == GBAR_TYPE_BUFFER) {
							need_gem = 0;
							break;
						}
					}
				}
			}
		}

		if (need_gem) {
			if (create_gem(buffer) < 0) {
				/* okay, something goes wrong */
			}
		}
	} else if (buffer->state != WIDGET_FB_STATE_CREATED || buffer->type != WIDGET_FB_TYPE_PIXMAP) {
		ErrPrint("Invalid buffer\n");
		return NULL;
	} else if (buffer->refcnt > 0) {
		buffer->refcnt++;
		return buffer;
	}

	s_info.pixmap_list = eina_list_append(s_info.pixmap_list, buffer);
	return buffer;
}

/*!
 * \return "buffer"
 */
EAPI void *buffer_handler_pixmap_find(int pixmap)
{
	widget_fb_t buffer;
	struct gem_data *gem;
	Eina_List *l;
	Eina_List *n;

	if (pixmap < 0) {
		return NULL;
	}

	EINA_LIST_FOREACH_SAFE(s_info.pixmap_list, l, n, buffer) {
		if (!buffer || buffer->state != WIDGET_FB_STATE_CREATED || buffer->type != WIDGET_FB_TYPE_PIXMAP) {
			s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);
			DbgPrint("Invalid buffer (List Removed: %p)\n", buffer);
			continue;
		}

		gem = (struct gem_data *)buffer->data;
		if (gem->pixmap == pixmap) {
			return buffer;
		}
	}

	return NULL;
}

EAPI int buffer_handler_pixmap_release_buffer(void *canvas)
{
	widget_fb_t buffer;
	struct gem_data *gem;
	Eina_List *l;
	Eina_List *n;
	void *_ptr;

	if (!canvas) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	EINA_LIST_FOREACH_SAFE(s_info.pixmap_list, l, n, buffer) {
		if (!buffer || buffer->state != WIDGET_FB_STATE_CREATED || buffer->type != WIDGET_FB_TYPE_PIXMAP) {
			s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);
			continue;
		}

		gem = (struct gem_data *)buffer->data;
		_ptr = gem->data;

		if (!_ptr) {
			continue;
		}

		if (_ptr == canvas) {
			release_gem(buffer);
			buffer_handler_pixmap_unref(buffer);
			return WIDGET_ERROR_NONE;
		}
	}

	return WIDGET_ERROR_NOT_EXIST;
}

/*!
 * \note
 *
 * \return Return NULL if the buffer is in still uses.
 * 	   Return buffer_ptr if it needs to destroy
 */
EAPI int buffer_handler_pixmap_unref(void *buffer_ptr)
{
	widget_fb_t buffer = buffer_ptr;
	struct buffer_info *info;

	buffer->refcnt--;
	if (buffer->refcnt > 0) {
		return WIDGET_ERROR_NONE; /* Return NULL means, gem buffer still in use */
	}

	s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);

	info = buffer->info;

	if (destroy_gem(buffer) < 0) {
		ErrPrint("Failed to destroy gem buffer\n");
	}

	if (info && info->buffer == buffer) {
		info->buffer = NULL;
	}

	if (destroy_pixmap(buffer) < 0) {
		ErrPrint("Failed to destroy pixmap\n");
	}

	return WIDGET_ERROR_NONE;
}

EAPI int buffer_handler_is_loaded(const struct buffer_info *info)
{
	return info ? info->is_loaded : 0;
}

EAPI void buffer_handler_update_size(struct buffer_info *info, int w, int h)
{
	if (!info) {
		return;
	}

	info->w = w;
	info->h = h;
}

EAPI int buffer_handler_resize(struct buffer_info *info, int w, int h)
{
	int ret;

	if (!info) {
		ErrPrint("Invalid handler\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (info->w == w && info->h == h) {
		DbgPrint("No changes\n");
		return WIDGET_ERROR_NONE;
	}

	buffer_handler_update_size(info, w, h);

	if (!info->is_loaded) {
		DbgPrint("Buffer size is updated[%dx%d]\n", w, h);
		return WIDGET_ERROR_NONE;
	}

	ret = buffer_handler_unload(info);
	if (ret < 0) {
		ErrPrint("Unload: %d\n", ret);
	}

	ret = buffer_handler_load(info);
	if (ret < 0) {
		ErrPrint("Load: %d\n", ret);
	}

	return WIDGET_ERROR_NONE;
}

EAPI int buffer_handler_get_size(struct buffer_info *info, int *w, int *h)
{
	if (!info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (w) {
		*w = info->w;
	}
	if (h) {
		*h = info->h;
	}

	return WIDGET_ERROR_NONE;
}

EAPI struct inst_info *buffer_handler_instance(struct buffer_info *info)
{
	return info->inst;
}

EAPI void buffer_handler_flush(struct buffer_info *info)
{
	int fd;
	int size;
	widget_fb_t buffer;

	if (!info || !info->buffer) {
		return;
	}

	buffer = info->buffer;

	if (buffer->type == WIDGET_FB_TYPE_PIXMAP) {
		/**
		 * @TODO
		 * WL_XXX
		 * Not supported for wayland or this should be ported correctly
		 */
	} else if (buffer->type == WIDGET_FB_TYPE_FILE) {
		fd = open(widget_util_uri_to_path(info->id), O_WRONLY | O_CREAT, 0644);
		if (fd < 0) {
			ErrPrint("%s open falied: %d\n", widget_util_uri_to_path(info->id), errno);
			return;
		}

		size = info->w * info->h * info->pixel_size;
		widget_service_acquire_lock(info->lock_info);
		if (write(fd, info->buffer, size) != size) {
			ErrPrint("write: %d\n", errno);
		}
		widget_service_release_lock(info->lock_info);

		if (close(fd) < 0) {
			ErrPrint("close: %d\n", errno);
		}
	} else {
		DbgPrint("Flush nothing\n");
	}
}

HAPI int buffer_handler_init(void)
{
	int ret;

	if (WIDGET_CONF_USE_SW_BACKEND) {
		DbgPrint("Fallback to the S/W Backend\n");
		return WIDGET_ERROR_NONE;
	}

	ret = widget_util_get_drm_fd(ecore_wl_display_get(), &s_info.fd);
	if (ret != WIDGET_ERROR_NONE || s_info.fd < 0) {
		ErrPrint("Fallback to the S/W Backend\n");
		return WIDGET_ERROR_NONE;
	}

	s_info.slp_bufmgr = tbm_bufmgr_init(s_info.fd);
	if (!s_info.slp_bufmgr) {
		widget_util_release_drm_fd(s_info.fd);
		s_info.fd = -1;
		return WIDGET_ERROR_NONE;
	}

	return WIDGET_ERROR_NONE;
}

HAPI int buffer_handler_fini(void)
{
	if (s_info.slp_bufmgr) {
		tbm_bufmgr_deinit(s_info.slp_bufmgr);
		s_info.slp_bufmgr = NULL;
	}

	if (s_info.fd >= 0) {
		widget_util_release_drm_fd(s_info.fd);
		s_info.fd = -1;
	}

	return WIDGET_ERROR_NONE;
}

static inline widget_fb_t raw_open_file(const char *filename)
{
	widget_fb_t buffer;
	int fd;
	off_t off;
	int ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		ErrPrint("open: %d\n", errno);
		return NULL;
	}

	off = lseek(fd, 0L, SEEK_END);
	if (off == (off_t)-1) {
		ErrPrint("lseek: %d\n", errno);

		if (close(fd) < 0) {
			ErrPrint("close: %d\n", errno);
		}

		return NULL;
	}

	if (lseek(fd, 0L, SEEK_SET) == (off_t)-1) {
		ErrPrint("lseek: %d\n", errno);

		if (close(fd) < 0) {
			ErrPrint("close: %d\n", errno);
		}

		return NULL;
	}

	buffer = calloc(1, sizeof(*buffer) + off);
	if (!buffer) {
		ErrPrint("calloc: %d\n", errno);

		if (close(fd) < 0) {
			ErrPrint("close: %d\n", errno);
		}

		return NULL;
	}

	buffer->state = WIDGET_FB_STATE_CREATED;
	buffer->type = WIDGET_FB_TYPE_FILE;
	buffer->refcnt = 0;
	buffer->info = (void *)off;

	ret = read(fd, buffer->data, off);
	if (ret < 0) {
		ErrPrint("read: %d\n", errno);
		DbgFree(buffer);

		if (close(fd) < 0) {
			ErrPrint("close: %d\n", errno);
		}

		return NULL;
	}

	if (close(fd) < 0) {
		ErrPrint("close: %d\n", errno);
	}

	return buffer;
}

static inline int raw_close_file(widget_fb_t buffer)
{
	DbgFree(buffer);
	return 0;
}

static inline widget_fb_t raw_open_shm(int shm)
{
	widget_fb_t buffer;

	buffer = (widget_fb_t)shmat(shm, NULL, SHM_RDONLY);
	if (buffer == (widget_fb_t )-1) {
		ErrPrint("shmat: %d\n", errno);
		return NULL;
	}

	return buffer;
}

static inline int raw_close_shm(widget_fb_t buffer)
{
	int ret;

	ret = shmdt(buffer);
	if (ret < 0) {
		ErrPrint("shmdt: %d\n", errno);
	}

	return ret;
}

static inline widget_fb_t raw_open_pixmap(unsigned int pixmap)
{
	widget_fb_t buffer;

	buffer = calloc(1, sizeof(*buffer) + WIDGET_CONF_DEFAULT_PIXELS);
	if (!buffer) {
		ErrPrint("calloc: %d\n", errno);
		return NULL;
	}

	buffer->state = WIDGET_FB_STATE_CREATED;
	buffer->type = WIDGET_FB_TYPE_PIXMAP;

	return buffer;
}

static inline int raw_close_pixmap(widget_fb_t buffer)
{
	DbgFree(buffer);
	return 0;
}

EAPI void *buffer_handler_raw_data(widget_fb_t buffer)
{
	if (!buffer || buffer->state != WIDGET_FB_STATE_CREATED) {
		return NULL;
	}

	return buffer->data;
}

EAPI int buffer_handler_raw_size(widget_fb_t buffer)
{
	if (!buffer || buffer->state != WIDGET_FB_STATE_CREATED) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	return (int)((long)buffer->info);
}

EAPI widget_fb_t buffer_handler_raw_open(enum widget_fb_type widget_fb_type, void *resource)
{
	widget_fb_t handle;

	switch (widget_fb_type) {
	case WIDGET_FB_TYPE_SHM:
		handle = raw_open_shm((int)((long)resource));
		break;
	case WIDGET_FB_TYPE_FILE:
		handle = raw_open_file(resource);
		break;
	case WIDGET_FB_TYPE_PIXMAP:
		handle = raw_open_pixmap((unsigned int)((long)resource));
		break;
	default:
		handle = NULL;
		break;
	}

	return handle;
}

EAPI int buffer_handler_raw_close(widget_fb_t buffer)
{
	int ret;

	if (!buffer || buffer->state != WIDGET_FB_STATE_CREATED) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (buffer->type) {
	case WIDGET_FB_TYPE_SHM:
		ret = raw_close_shm(buffer);
		break;
	case WIDGET_FB_TYPE_FILE:
		ret = raw_close_file(buffer);
		break;
	case WIDGET_FB_TYPE_PIXMAP:
		ret = raw_close_pixmap(buffer);
		break;
	default:
		ret = WIDGET_ERROR_INVALID_PARAMETER;
		break;
	}

	return ret;
}

EAPI int buffer_handler_lock(struct buffer_info *info)
{
	if (!info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (info->type == WIDGET_FB_TYPE_PIXMAP) {
		return WIDGET_ERROR_NONE;
	}

	if (info->type == WIDGET_FB_TYPE_FILE) {
		return WIDGET_ERROR_NONE;
	}

	return widget_service_acquire_lock(info->lock_info);
}

EAPI int buffer_handler_unlock(struct buffer_info *info)
{
	if (!info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	if (info->type == WIDGET_FB_TYPE_PIXMAP) {
		return WIDGET_ERROR_NONE;
	}

	if (info->type == WIDGET_FB_TYPE_FILE) {
		return WIDGET_ERROR_NONE;
	}

	return widget_service_release_lock(info->lock_info);
}

EAPI int buffer_handler_auto_align(void)
{
	return 1;
}

EAPI int buffer_handler_stride(struct buffer_info *info)
{
	int stride;

	if (!info) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	switch (info->type) {
	case WIDGET_FB_TYPE_FILE:
	case WIDGET_FB_TYPE_SHM:
	case WIDGET_FB_TYPE_PIXMAP:
		stride = info->w * info->pixel_size;
		break;
	default:
		stride = WIDGET_ERROR_INVALID_PARAMETER;
		break;
	}

	return stride;
}

/*!
 * \note
 * Only can be used by master.
 * Plugin cannot access the user data
 */

HAPI int buffer_handler_set_data(struct buffer_info *buffer, void *data)
{
	if (!buffer) {
		ErrPrint("Invalid handle\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	buffer->data = data;
	return WIDGET_ERROR_NONE;
}

HAPI void *buffer_handler_data(struct buffer_info *buffer)
{
	if (!buffer) {
		ErrPrint("Invalid handle\n");
		return NULL;
	}

	return buffer->data;
}

HAPI int buffer_handler_destroy(struct buffer_info *info)
{
	Eina_List *l;
	widget_fb_t buffer;

	if (!info) {
		DbgPrint("Buffer is not created yet. info is NIL\n");
		return WIDGET_ERROR_NONE;
	}

	EINA_LIST_FOREACH(s_info.pixmap_list, l, buffer) {
		if (buffer->info == info) {
			buffer->info = NULL;
		}
	}

	buffer_handler_unload(info);
	DbgFree(info->id);
	DbgFree(info);
	return WIDGET_ERROR_NONE;
}

HAPI struct buffer_info *buffer_handler_create(struct inst_info *inst, enum widget_fb_type type, int w, int h, int pixel_size)
{
	struct buffer_info *info;

	info = malloc(sizeof(*info));
	if (!info) {
		ErrPrint("malloc: %d\n", errno);
		return NULL;
	}

	switch (type) {
	case WIDGET_FB_TYPE_SHM:
		if (pixel_size != WIDGET_CONF_DEFAULT_PIXELS) {
			DbgPrint("SHM only supportes %d bytes pixels (requested: %d)\n", WIDGET_CONF_DEFAULT_PIXELS, pixel_size);
			pixel_size = WIDGET_CONF_DEFAULT_PIXELS;
		}

		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(info);
			return NULL;
		}
		break;
	case WIDGET_FB_TYPE_FILE:
		if (pixel_size != WIDGET_CONF_DEFAULT_PIXELS) {
			DbgPrint("FILE only supportes %d bytes pixels (requested: %d)\n", WIDGET_CONF_DEFAULT_PIXELS, pixel_size);
			pixel_size = WIDGET_CONF_DEFAULT_PIXELS;
		}

		info->id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
		if (!info->id) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(info);
			return NULL;
		}
		break;
	case WIDGET_FB_TYPE_PIXMAP:
		info->id = strdup(SCHEMA_PIXMAP "0:0");
		if (!info->id) {
			ErrPrint("strdup: %d\n", errno);
			DbgFree(info);
			return NULL;
		}
		break;
	default:
		ErrPrint("Invalid type\n");
		DbgFree(info);
		return NULL;
	}

	info->lock_info = NULL;
	info->w = w;
	info->h = h;
	info->pixel_size = pixel_size;
	info->type = type;
	info->is_loaded = 0;
	info->inst = inst;
	info->buffer = NULL;
	info->data = NULL;

	return info;
}

/* End of a file */
