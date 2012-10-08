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
#include <Ecore_X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

#include <dri2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_slp_bufmgr.h>

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
	void *info;
	char data[];
};

/*!
 * \brief Allocate this in the buffer->data.
 */
struct gem_data {
	DRI2Buffer *dri2_buffer;
	unsigned int attachments[1];
	drm_slp_bo pixmap_bo;
	int count;
	int buf_count;
	int w;
	int h;
	int depth;
	Pixmap pixmap;
	void *data; /* Gem layer */
	int refcnt;
};

struct buffer_info
{
	void *buffer;
	char *id;

	enum buffer_type type;

	int w;
	int h;
	int pixel_size;
	int is_loaded;

	struct inst_info *inst;
};

static struct {
	drm_slp_bufmgr slp_bufmgr;
	int evt_base;
	int err_base;
	int fd;
	Eina_List *pixmap_list;
} s_info = {
	.slp_bufmgr = NULL,
	.evt_base = 0,
	.err_base = 0,
	.fd = -1,
	.pixmap_list = NULL,
};

struct buffer_info *buffer_handler_create(struct inst_info *inst, enum buffer_type type, int w, int h, int pixel_size)
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
		info->id = strdup(SCHEMA_PIXMAP "0");
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
	info->inst = inst;
	info->buffer = NULL;

	return info;
}

static inline struct buffer *create_gem(Display *disp, Window parent, int w, int h, int depth)
{
	struct gem_data *gem;
	struct buffer *buffer;

	if (!s_info.slp_bufmgr)
		return NULL;

	buffer = calloc(1, sizeof(*buffer) + sizeof(*gem));
	if (!buffer) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	gem = (struct gem_data *)buffer->data;

	buffer->type = BUFFER_TYPE_PIXMAP;
	buffer->refcnt = 1;
	buffer->state = CREATED;

	DbgPrint("Canvas %dx%d - %d is created\n", w, h, depth);

	gem->attachments[0] = DRI2BufferFrontLeft;
	gem->count = 1;
	gem->w = w;
	gem->h = h;
	gem->depth = depth;
	/*!
	 * \NOTE
	 * Use the 24 Bits
	 * 32 Bits is not supported for video playing.
	 */
	gem->pixmap = XCreatePixmap(disp, parent, w, h, 24 /* (depth << 3) */);
	if (gem->pixmap == (Pixmap)0) {
		ErrPrint("Failed to create a pixmap\n");
		free(buffer);
		return NULL;
	}

	XSync(disp, False);

	DbgPrint("Pixmap:0x%X is created\n", gem->pixmap);

	DRI2CreateDrawable(disp, gem->pixmap);

	DbgPrint("DRI2CreateDrawable is done\n");
	gem->dri2_buffer = DRI2GetBuffers(disp, gem->pixmap,
					&gem->w, &gem->h, gem->attachments, gem->count, &gem->buf_count);
	DbgPrint("dri2_buffer: %p, name: %p, %dx%d (%dx%d)\n", gem->dri2_buffer, gem->dri2_buffer->name, gem->w, gem->h, w, h);
	DbgPrint("dri2_buffer->pitch : %d, buf_count: %d\n", gem->dri2_buffer->pitch, gem->buf_count);
	if (!gem->dri2_buffer || !gem->dri2_buffer->name) {
		ErrPrint("Failed to get GemBuffer\n");
		XFreePixmap(disp, gem->pixmap);
		buffer->state = DESTROYED;
		free(buffer);
		return NULL;
	}

	/*!
	 * \How can I destroy this?
	 */
	gem->pixmap_bo = drm_slp_bo_import(s_info.slp_bufmgr, gem->dri2_buffer->name);
	if (!gem->pixmap_bo) {
		DRI2DestroyDrawable(disp, gem->pixmap);
		XFreePixmap(disp, gem->pixmap);
		ErrPrint("Failed to import BO\n");
		buffer->state = DESTROYED;
		free(buffer);
		return NULL;
	}

	DbgPrint("Return buffer: %p\n", buffer);
	return buffer;
}

static inline void *acquire_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	if (!buffer)
		return NULL;

	gem = (struct gem_data *)buffer->data;
	if (!gem->data) {
		if (gem->refcnt) {
			ErrPrint("Already acquired. but the buffer is not valid\n");
			return NULL;
		}

		gem->data = (void *)drm_slp_bo_map(gem->pixmap_bo, DRM_SLP_DEVICE_CPU, DRM_SLP_OPTION_READ|DRM_SLP_OPTION_WRITE);
	}

	gem->refcnt++;

	DbgPrint("gem->data %p is gotten (%d)\n", gem->data, gem->refcnt);
	return gem->data;
}

static inline void release_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	gem = (struct gem_data *)buffer->data;
	if (!gem->data) {
		if (gem->refcnt > 0) {
			ErrPrint("Reference count is not valid %d\n", gem->refcnt);
			gem->refcnt = 0;
		}
		return;
	}

	gem->refcnt--;
	if (gem->refcnt == 0) {
		gem = (struct gem_data *)buffer->data;
		DbgPrint("Release gem : %d (%p)\n", buffer->refcnt, gem->data);
		drm_slp_bo_unmap(gem->pixmap_bo, DRM_SLP_DEVICE_CPU);
		gem->data = NULL;
	} else if (gem->refcnt < 0) {
		DbgPrint("Invalid refcnt: %d (reset)\n", gem->refcnt);
		gem->refcnt = 0;
	}
}

static inline int destroy_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	if (!buffer)
		return -EINVAL;

	/*!
	 * Forcely release the acquire_buffer.
	 */
	gem = (struct gem_data *)buffer->data;
	if (!gem)
		return -EFAULT;

	DbgPrint("unref pixmap bo\n");
	drm_slp_bo_unref(gem->pixmap_bo);
	gem->pixmap_bo = NULL;

	DbgPrint("DRI2DestroyDrawable\n");
	DRI2DestroyDrawable(ecore_x_display_get(), gem->pixmap);
	DbgPrint("After destroy drawable\n");

	XFreePixmap(ecore_x_display_get(), gem->pixmap);
	DbgPrint("Free pixmap 0x%X\n", gem->pixmap);

	buffer->state = DESTROYED;
	free(buffer);
	return 0;
}

static inline int load_file_buffer(struct buffer_info *info)
{
	struct buffer *buffer;
	double timestamp;
	int size;
	char *new_id;
	int len;

	len = strlen(IMAGE_PATH) + 40;
	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	timestamp = util_timestamp();
	snprintf(new_id, len, SCHEMA_FILE "%s%lf", IMAGE_PATH, timestamp);

	size = sizeof(*buffer) + info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Canvas buffer size is ZERO\n");
		free(new_id);
		return -EINVAL;
	}

	buffer = calloc(1, size);
	if (!buffer) {
		ErrPrint("Failed to allocate buffer\n");
		free(new_id);
		return -ENOMEM;
	}

	buffer->type = BUFFER_TYPE_FILE;
	buffer->refcnt = 0;
	buffer->state = CREATED;
	buffer->info = info;

	free(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;

	DbgPrint("FILE type %d created\n", size);
	return 0;
}

static inline int load_shm_buffer(struct buffer_info *info)
{
	int id;
	int size;
	struct buffer *buffer; /* Just for getting a size */
	char *new_id;
	int len;

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

	buffer = shmat(id, NULL, 0);
	if (buffer == (void *)-1) {
		ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

		return -EFAULT;
	}

	buffer->type = BUFFER_TYPE_SHM;
	buffer->refcnt = id;
	buffer->state = CREATED; /*!< Needless */
	buffer->info = NULL; /*!< This has not to be used, every process will see this. So, don't try to save anything on here */

	len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */

	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		if (shmdt(buffer) < 0)
			ErrPrint("shmdt: %s\n", strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("shmctl: %s\n", strerror(errno));

		return -ENOMEM;
	}

	snprintf(new_id, len, SCHEMA_SHM "%d", id);

	free(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;
	return 0;
}

static inline int load_pixmap_buffer(struct buffer_info *info)
{
	struct buffer *buffer;
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

	if (info->buffer)
		DbgPrint("Buffer is already exists, but override it with new one\n");

	buffer = buffer_handler_pixmap_ref(info);
	if (!buffer) {
		DbgPrint("Failed to make a reference of a pixmap\n");
		info->is_loaded = 0;
		return -EFAULT;
	}

	len = strlen(SCHEMA_PIXMAP) + 30; /* strlen("pixmap://") + 30 */
	new_id = malloc(len);
	if (!new_id) {
		info->is_loaded = 0;
		ErrPrint("Heap: %s\n", strerror(errno));
		buffer_handler_pixmap_unref(buffer);
		return -ENOMEM;
	}

	DbgPrint("Releaseo old id (%s)\n", info->id);
	free(info->id);
	info->id = new_id;

	gem = (struct gem_data *)buffer->data;
	DbgPrint("gem pointer: %p\n", gem);

	snprintf(info->id, len, SCHEMA_PIXMAP "%d", (int)gem->pixmap);
	DbgPrint("info->id: %s\n", info->id);

	return 0;
}

int buffer_handler_load(struct buffer_info *info)
{
	int ret;

	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (info->is_loaded) {
		DbgPrint("Buffer is already loaded\n");
		return 0;
	}

	switch (info->type) {
	case BUFFER_TYPE_FILE:
		ret = load_file_buffer(info);
		break;
	case BUFFER_TYPE_SHM:
		ret = load_shm_buffer(info);
		break;
	case BUFFER_TYPE_PIXMAP:
		ret = load_pixmap_buffer(info);
		break;
	default:
		ErrPrint("Invalid buffer\n");
		ret = -EINVAL;
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
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	free(info->buffer);
	info->buffer = NULL;

	path = util_uri_to_path(info->id);
	if (path && unlink(path) < 0)
		ErrPrint("unlink: %s\n", strerror(errno));

	free(info->id);
	info->id = new_id;
	return 0;
}

static inline int unload_shm_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;

	new_id = strdup(SCHEMA_SHM "-1");
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (sscanf(info->id, SCHEMA_SHM "%d", &id) != 1) {
		ErrPrint("%s Invalid ID\n", info->id);
		free(new_id);
		return -EINVAL;
	}

	if (id < 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		free(new_id);
		return -EINVAL;
	}

	if (shmdt(info->buffer) < 0)
		ErrPrint("Detach shm: %s\n", strerror(errno));

	if (shmctl(id, IPC_RMID, 0) < 0)
		ErrPrint("Remove shm: %s\n", strerror(errno));

	info->buffer = NULL;

	free(info->id);
	info->id = new_id;
	return 0;
}

static inline int unload_pixmap_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;

	new_id = strdup(SCHEMA_PIXMAP "0");
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (sscanf(info->id, SCHEMA_PIXMAP "%d", &id) != 1) {
		ErrPrint("Invalid ID (%s)\n", info->id);
		free(new_id);
		return -EINVAL;
	}

	if (id == 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		free(new_id);
		return -EINVAL;
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

	free(info->id);
	info->id = new_id;
	return 0;
}

int buffer_handler_unload(struct buffer_info *info)
{
	int ret;

	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return -EINVAL;
	}

	switch (info->type) {
	case BUFFER_TYPE_FILE:
		ret = unload_file_buffer(info);
		break;
	case BUFFER_TYPE_SHM:
		ret = unload_shm_buffer(info);
		break;
	case BUFFER_TYPE_PIXMAP:
		ret = unload_pixmap_buffer(info);
		break;
	default:
		ErrPrint("Invalid buffer\n");
		ret = -EINVAL;
		break;
	}

	if (ret == 0)
		info->is_loaded = 0;

	return ret;
}

int buffer_handler_destroy(struct buffer_info *info)
{
	Eina_List *l;
	struct buffer *buffer;

	if (!info) {
		DbgPrint("Buffer is not created yet. info is nil\n");
		return 0;
	}

	EINA_LIST_FOREACH(s_info.pixmap_list, l, buffer) {
		if (buffer->info == info)
			buffer->info = NULL;
	}

	buffer_handler_unload(info);
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

void *buffer_handler_fb(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info)
		return NULL;

	buffer = info->buffer;

	if (info->type == BUFFER_TYPE_PIXMAP) {
		void *canvas;
		int ret;

		canvas = buffer_handler_pixmap_acquire_buffer(info);
		ret = buffer_handler_pixmap_release_buffer(canvas);
		DbgPrint("Canvas %p(%d) (released but still in use)\n", canvas, ret);
		return canvas;
	}

	return buffer->data;
}

int buffer_handler_pixmap(const struct buffer_info *info)
{
	struct buffer *buf;
	struct gem_data *gem;

	if (!info) {
		ErrPrint("Inavlid buffer handler\n");
		return 0;
	}

	if (info->type != BUFFER_TYPE_PIXMAP) {
		ErrPrint("Invalid buffer type\n");
		return 0;
	}

	buf = (struct buffer *)info->buffer;
	if (!buf) {
		ErrPrint("Invalid buffer data\n");
		return 0;
	}

	gem = (struct gem_data *)buf->data;
	return gem->pixmap;
}

void *buffer_handler_pixmap_acquire_buffer(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	buffer = buffer_handler_pixmap_ref(info);
	if (!buffer)
		return NULL;

	return acquire_gem(buffer);
}

void *buffer_handler_pixmap_buffer(struct buffer_info *info)
{
	struct buffer *buffer;
	struct gem_data *gem;

	if (!info)
		return NULL;

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	buffer = info->buffer;
	if (!buffer)
		return NULL;

	gem = (struct gem_data *)buffer->data;
	return gem->data;
}

/*!
 * \return "buffer" object (Not the buffer_info)
 */
void *buffer_handler_pixmap_ref(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	if (info->type != BUFFER_TYPE_PIXMAP) {
		ErrPrint("Buffer type is not matched\n");
		return NULL;
	}

	buffer = info->buffer;
	if (!buffer) {
		Display *disp;
		Window root;

		disp = ecore_x_display_get();
		root = DefaultRootWindow(disp);

		buffer = create_gem(disp, root, info->w, info->h, info->pixel_size);
		if (!buffer) {
			DbgPrint("No GEM initialization\n");
			return NULL;
		}

		DbgPrint("Buffer is created: %p\n", buffer);
		info->buffer = buffer;
	} else if (buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP) {
		ErrPrint("Invalid buffer\n");
		return NULL;
	} else if (buffer->refcnt > 0) {
		buffer->refcnt++;
		DbgPrint("Increase the refcnt - Return buffer %p\n", buffer);
		return buffer;
	}

	s_info.pixmap_list = eina_list_append(s_info.pixmap_list, buffer);
	DbgPrint("(List appended - Return buffer %p\n", buffer);
	return buffer;
}

/*!
 * \return "buffer"
 */
void *buffer_handler_pixmap_find(int pixmap)
{
	struct buffer *buffer;
	struct gem_data *gem;
	Eina_List *l;
	Eina_List *n;

	if (pixmap == 0)
		return NULL;

	EINA_LIST_FOREACH_SAFE(s_info.pixmap_list, l, n, buffer) {
		if (!buffer || buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP) {
			s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);
			DbgPrint("Invalid buffer (List Removed: %p)\n", buffer);
			continue;
		}

		gem = (struct gem_data *)buffer->data;
		if (gem->pixmap == pixmap)
			return buffer;
	}

	return NULL;
}

int buffer_handler_pixmap_release_buffer(void *canvas)
{
	struct buffer *buffer;
	struct gem_data *gem;
	Eina_List *l;
	Eina_List *n;
	void *_ptr;

	if (!canvas)
		return -EINVAL;

	EINA_LIST_FOREACH_SAFE(s_info.pixmap_list, l, n, buffer) {
		if (!buffer || buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP) {
			s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);
			continue;
		}

		gem = (struct gem_data *)buffer->data;
		_ptr = gem->data;

		if (!_ptr)
			continue;
		
		if (_ptr == canvas) {
			release_gem(buffer);
			buffer_handler_pixmap_unref(buffer);
			return 0;
		}
	}

	return -ENOENT;
}

/*!
 * \note
 *
 * \return Return NULL if the buffer is in still uses.
 * 	   Return buffer_ptr if it needs to destroy
 */
int buffer_handler_pixmap_unref(void *buffer_ptr)
{
	struct buffer *buffer = buffer_ptr;
	struct buffer_info *info;

	buffer->refcnt--;
	if (buffer->refcnt > 0)
		return 0; /* Return NULL means, gem buffer still in use */

	s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);

	destroy_gem(buffer);

	info = buffer->info;
	if (info && info->buffer == buffer)
		info->buffer = NULL;

	return 0;
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
	int ret;

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

	ret = buffer_handler_unload(info);
	if (ret < 0)
		ErrPrint("Unload: %d\n", ret);

	ret = buffer_handler_load(info);
	if (ret < 0)
		ErrPrint("Load: %d\n", ret);

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

struct inst_info *buffer_handler_instance(struct buffer_info *info)
{
	return info->inst;
}

void buffer_handler_flush(struct buffer_info *info)
{
	int fd;
	int size;
	struct buffer *buffer;

	if (!info || !info->buffer)
		return;

	buffer = info->buffer;

	if (buffer->type == BUFFER_TYPE_PIXMAP) {
		XRectangle rect;
		XserverRegion region;

		rect.x = 0;
		rect.y = 0;
		rect.width = info->w;
		rect.height = info->h;

		region = XFixesCreateRegion(ecore_x_display_get(), &rect, 1);
		XDamageAdd(ecore_x_display_get(), buffer_handler_pixmap(info), region);
		XFixesDestroyRegion(ecore_x_display_get(), region);
		XFlush(ecore_x_display_get());
	} else if (buffer->type == BUFFER_TYPE_FILE) {
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
	} else {
		DbgPrint("Flush nothing\n");
	}
}

int buffer_handler_init(void)
{
	int dri2Major, dri2Minor;
	char *driverName, *deviceName;
	drm_magic_t magic;

	if (!DRI2QueryExtension(ecore_x_display_get(), &s_info.evt_base, &s_info.err_base)) {
		DbgPrint("DRI2 is not supported\n");
		return 0;
	}

	if (!DRI2QueryVersion(ecore_x_display_get(), &dri2Major, &dri2Minor)) {
		DbgPrint("DRI2 is not supported\n");
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	if (!DRI2Connect(ecore_x_display_get(), DefaultRootWindow(ecore_x_display_get()), &driverName, &deviceName)) {
		DbgPrint("DRI2 is not supported\n");
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	DbgPrint("Open: %s", deviceName);
	s_info.fd = open(deviceName, O_RDWR);
	if (s_info.fd < 0) {
		DbgPrint("Failed to open a drm device: %s (%s)\n", deviceName, strerror(errno));
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	drmGetMagic(s_info.fd, &magic);
	DbgPrint("DRM Magic: 0x%X\n", magic);
	if (!DRI2Authenticate(ecore_x_display_get(), DefaultRootWindow(ecore_x_display_get()), (unsigned int)magic)) {
		DbgPrint("Failed to do authenticate for DRI2\n");
		close(s_info.fd);
		s_info.fd = -1;
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	s_info.slp_bufmgr = drm_slp_bufmgr_init(s_info.fd, NULL);
	if (!s_info.slp_bufmgr) {
		DbgPrint("Failed to init bufmgr\n");
		close(s_info.fd);
		s_info.fd = -1;
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	return 0;
}

int buffer_handler_fini(void)
{
	if (s_info.fd >= 0) {
		close(s_info.fd);
		s_info.fd = -1;
	}

	if (s_info.slp_bufmgr) {
		drm_slp_bufmgr_destroy(s_info.slp_bufmgr);
		s_info.slp_bufmgr = NULL;
	}

	return 0;
}

/* End of a file */
