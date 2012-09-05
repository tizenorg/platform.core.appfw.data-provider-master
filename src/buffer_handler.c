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

	Ecore_Event_Handler *damage_handler;
	Ecore_X_Damage damage;
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
} s_info = {
	.slp_bufmgr = NULL,
	.evt_base = 0,
	.err_base = 0,
	.fd = -1,
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
	buffer->refcnt = 0;
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

	return buffer;
}

static inline void *acquire_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	gem = (struct gem_data *)buffer->data;

	gem->data = (void *)drm_slp_bo_map(gem->pixmap_bo, DRM_SLP_DEVICE_CPU, DRM_SLP_OPTION_READ|DRM_SLP_OPTION_WRITE);
	if (gem->data)
		buffer->refcnt++;

	DbgPrint("gem->data %p is gotten\n", gem->data);
	return gem->data;
}

static inline void release_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	gem = (struct gem_data *)buffer->data;

	DbgPrint("Release gem : %d (%p)\n", buffer->refcnt, gem->data);
	if (buffer->refcnt == 0)
		return;

	drm_slp_bo_unmap(gem->pixmap_bo, DRM_SLP_DEVICE_CPU);
	gem->data = NULL;
}

static inline int destroy_gem(Display *disp, struct buffer *buffer)
{
	struct gem_data *gem;

	if (!buffer)
		return -EINVAL;

	/*!
	 * Forcely release the acquire_buffer.
	 */
	gem = (struct gem_data *)buffer->data;

	release_gem(buffer);

	DbgPrint("unref pixmap bo\n");
	drm_slp_bo_unref(gem->pixmap_bo);
	gem->pixmap_bo = NULL;

	DbgPrint("DRI2DestroyDrawable\n");
	DRI2DestroyDrawable(disp, gem->pixmap);
	DbgPrint("After destroy drawable\n");

	XFreePixmap(disp, gem->pixmap);
	DbgPrint("Free pixmap\n");

	buffer->state = DESTROYED;
	free(buffer);
	return 0;
}

static Eina_Bool damage_event_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Damage *e = (Ecore_X_Event_Damage *)event;
	struct buffer_info *info;
	struct buffer *buffer;
	struct gem_data *gem;

	info = (struct buffer_info *)data;
	if (!info)
		return ECORE_CALLBACK_PASS_ON;

	buffer = (struct buffer *)info->buffer;
	if (!buffer)
		return ECORE_CALLBACK_PASS_ON;

	gem = (struct gem_data *)buffer->data;

	DbgPrint("0x%X <> 0x%X\n", e->drawable, gem->pixmap);
	if (e->drawable == gem->pixmap) {
		if (instance_pd_buffer(info->inst) == info) {
			instance_pd_updated_by_instance(info->inst, NULL);
		} else if (instance_lb_buffer(info->inst) == info) {
			instance_lb_updated_by_instance(info->inst);
		} else {
			ErrPrint("What happens?\n");
		}

		ecore_x_damage_subtract(gem->damage, None, None);
	}

	return ECORE_CALLBACK_PASS_ON;
}

int buffer_handler_load(struct buffer_info *info, int with_update_cb)
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
		buffer = calloc(1, size);
		if (!buffer) {
			ErrPrint("Failed to allocate buffer\n");
			return -ENOMEM;
		}

		buffer->type = BUFFER_TYPE_FILE;
		buffer->refcnt = 0;
		buffer->state = CREATED;

		len = strlen(g_conf.path.image) + 40;
		timestamp = util_timestamp();

		free(info->id);

		info->id = malloc(len);
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			buffer->state = DESTROYED;
			free(buffer);
			return -ENOMEM;
		}

		snprintf(info->id, len, SCHEMA_FILE "%s%lf", g_conf.path.image, timestamp);
		info->buffer = buffer;
		buffer->info = info;
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

		free(info->id);

		len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */
		info->id = malloc(len);
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			if (shmdt(buffer) < 0)
				ErrPrint("shmdt: %s\n", strerror(errno));

			if (shmctl(id, IPC_RMID, 0) < 0)
				ErrPrint("shmctl: %s\n", strerror(errno));

			return -ENOMEM;
		}

		snprintf(info->id, len, SCHEMA_SHM "%d", id);
		info->buffer = buffer;
		buffer->info = NULL; /*!< This has not to be used */
	} else if (info->type == BUFFER_TYPE_PIXMAP) {
		/*
		 */
		Display *disp;
		Window root;
		struct buffer *buffer;
		struct gem_data *gem;

		disp = ecore_x_display_get();
		root = DefaultRootWindow(disp);

		info->buffer = create_gem(disp, root, info->w, info->h, info->pixel_size);
		if (!info->buffer)
			DbgPrint("No GEM initialized\n");

		free(info->id);

		len = strlen(SCHEMA_PIXMAP) + 30; /* strlen("pixmap://") + 30 */
		info->id = malloc(len);
		if (!info->id) {
			destroy_gem(disp, info->buffer);
			info->buffer = NULL;
			return -ENOMEM;
		}

		buffer = info->buffer;
		buffer->info = info;

		gem = (struct gem_data *)buffer->data;
		snprintf(info->id, len, SCHEMA_PIXMAP "%d", (int)gem->pixmap);
		DbgPrint("info->id: %s\n", info->id);

		if (with_update_cb) {
			DbgPrint("Enable the damage event handler\n");
			gem->damage_handler = ecore_event_handler_add(ECORE_X_EVENT_DAMAGE_NOTIFY, damage_event_cb, info);
			if (!gem->damage_handler)
				ErrPrint("Failed to add a damage event handler\n");

			gem->damage = ecore_x_damage_new(gem->pixmap, ECORE_X_DAMAGE_REPORT_RAW_RECTANGLES);
			if (!gem->damage)
				ErrPrint("Failed to create a new damage\n");
		}
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
			ErrPrint("%s Invalid ID\n", info->id);
			return -EINVAL;
		}

		if (id < 0) {
			ErrPrint("(%s) Invalid id: %d\n", info->id, id);
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
		int id;
		struct buffer *buffer;
		struct gem_data *gem;

		if (sscanf(info->id, SCHEMA_PIXMAP "%d", &id) != 1) {
			ErrPrint("Invalid ID (%s)\n", info->id);
			return -EINVAL;
		}

		if (id == 0) {
			ErrPrint("(%s) Invalid id: %d\n", info->id, id);
			return -EINVAL;
		}

		buffer = (struct buffer *)info->buffer;
		if (buffer) {
			gem = (struct gem_data *)buffer->data;
			if (gem) {
				if (gem->damage) {
					ecore_x_damage_free(gem->damage);
					gem->damage = 0;
				}

				if (gem->damage_handler) {
					ecore_event_handler_del(gem->damage_handler);
					gem->damage_handler = NULL;
				}
			}
		}

		destroy_gem(ecore_x_display_get(), info->buffer);
		info->buffer = NULL;

		free(info->id);

		info->id = strdup(SCHEMA_PIXMAP "0");
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
	if (!info) {
		DbgPrint("Buffer is not created yet. info is nil\n");
		return 0;
	}

	if (info->type == BUFFER_TYPE_SHM) {
		buffer_handler_unload(info);
	} else if (info->type == BUFFER_TYPE_FILE) {
		unlink(info->id);
	} else if (info->type == BUFFER_TYPE_PIXMAP) {
		buffer_handler_unload(info);
	} else {
		DbgPrint("Buffer info: unknown type\n");
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
	struct buffer *buffer;

	if (!info)
		return NULL;

	buffer = info->buffer;

	if (!strncasecmp(info->id, SCHEMA_PIXMAP, strlen(SCHEMA_PIXMAP))) {
		void *canvas;

		canvas = buffer_handler_pixmap_acquire_buffer(info);
		buffer_handler_pixmap_release_buffer(info);
		DbgPrint("Canvas %p\n", canvas);
		return canvas;
	}

	return buffer->data;
}

int buffer_handler_pixmap(const struct buffer_info *info)
{
	int id;

	if (sscanf(info->id, SCHEMA_PIXMAP "%d", &id) != 1)
		return 0;

	return id;
}

void *buffer_handler_pixmap_acquire_buffer(const struct buffer_info *info)
{
	struct buffer *buffer;
	struct gem_data *gem;

	if (!info->is_loaded)
		return NULL;

	buffer = info->buffer;
	if (!buffer || buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP)
		return NULL;

	gem = (struct gem_data *)buffer->data;

	if (buffer->refcnt > 0) {
		DbgPrint("gem->data already exists: %p\n", gem->data);
		buffer->refcnt++;
		return gem->data;
	}

	return acquire_gem(buffer);
}

int buffer_handler_pixmap_release_buffer(const struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info->is_loaded)
		return -EINVAL;

	buffer = info->buffer;
	if (!buffer || buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP)
		return -EINVAL;

	buffer->refcnt--;
	if (buffer->refcnt < 0) {
		buffer->refcnt = 0;
		return -EINVAL;
	}

	if (buffer->refcnt > 0)
		return 0;

	release_gem(buffer);
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

	ret = buffer_handler_load(info, 0);
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

void buffer_handler_flush(struct buffer_info *info)
{
	int fd;
	int size;
	struct buffer *buffer;

	if (!info || !info->buffer)
		return;

	buffer = info->buffer;

	if (buffer->type == BUFFER_TYPE_PIXMAP) {
		DbgPrint("PIXMAP: Nothing to be done\n");
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
