#include <stdio.h>
#include <libgen.h> /* basename */
#include <errno.h>
#include <stdlib.h> /* free */
#include <ctype.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>

#include <Ecore_Evas.h>
#include <Evas.h>

#include <gio/gio.h>
#include <dlog.h>

#include "pkg_manager.h"
#include "client_manager.h"
#include "slave_manager.h"
#include "script_handler.h"
#include "fb.h"
#include "debug.h"
#include "conf.h"
#include "util.h"

#define TYPE_TEXT "text"
#define TYPE_IMAGE "image"
#define TYPE_EDJE "script"
#define TYPE_SIGNAL "signal"
#define TYPE_INFO "info"
#define TYPE_DRAG "drag"
#define INFO_SIZE "size"
#define INFO_CATEGORY "category"

static struct info {
	Eina_List *script_port_list;
} s_info = {
	.script_port_list = NULL,
};

struct script_port {
	void *handle;

	const char *(*magic_id)(void);
	int (*update_text)(void *handle, Evas *e, const char *id, const char *part, const char *text);
	int (*update_image)(void *handle, Evas *e, const char *id, const char *part, const char *path);
	int (*update_script)(void *handle, Evas *e, const char *id, const char *part, const char *path, const char *group);
	int (*update_signal)(void *handle, Evas *e, const char *id, const char *part, const char *signal);
	int (*update_drag)(void *handle, Evas *e, const char *id, const char *part, double x, double y);
	int (*update_size)(void *handle, Evas *e, const char *id, double w, double h);
	int (*update_category)(void *handle, Evas *e, const char *id, const char *category);

	void *(*create)(const char *file, const char *group, int w, int h);
	int (*destroy)(void *handle);

	int (*load)(void *handle, Evas *e);
	int (*unload)(void *handle, Evas *e);

	int (*init)(void);
	int (*fini)(void);
};

struct block {
	char *type;
	int type_len;

	char *part;
	int part_len;

	char *data;
	int data_len;

	char *file;
	int file_len;

	char *group;
	int group_len;

	char *id;
	int id_len;
};

struct script_info {
	Ecore_Evas *ee;
	struct fb_info *fb;
	struct inst_info *inst;
	int loaded;

	struct script_port *port;
	void *port_data;
};

static inline struct script_port *find_port(const char *magic_id)
{
	Eina_List *l;
	struct script_port *item;

	EINA_LIST_FOREACH(s_info.script_port_list, l, item) {
		if (!strcmp(item->magic_id(), magic_id))
			return item;
	}

	return NULL;
}

static void render_post_cb(void *data, Evas *e, void *event_info)
{
	struct inst_info *inst;
	struct script_info *info;

	inst = data;

	info = pkgmgr_lb_script(inst);
	if (info && script_handler_evas(info) == e) {
		fb_sync(script_handler_fb(info));
		pkgmgr_lb_updated_by_inst(inst);
		return;
	}

	info = pkgmgr_pd_script(inst);
	if (info && script_handler_evas(info) == e) {
		fb_sync(script_handler_fb(info));
		pkgmgr_pd_updated_by_inst(inst, NULL);
		return;
	}

	ErrPrint("Failed to sync\n");
	return;
}

int script_signal_emit(Evas *e, const char *part, const char *signal)
{
	Ecore_Evas *ee;
	struct script_info *info;
	const char *pkgname;
	const char *filename;
	struct slave_node *slave;
	GVariant *param;

	ee = ecore_evas_ecore_evas_get(e);
	if (!ee) {
		ErrPrint("Evas has no Ecore_Evas\n");
		return -EINVAL;
	}

	info = ecore_evas_data_get(ee, "script,info");
	if (!info) {
		ErrPrint("ecore_evas doesn't carry info data\n");
		return -EINVAL;
	}

	if (!signal || strlen(signal) == 0)
		signal = "";
	if (!part || strlen(part) == 0)
		part = "";

	pkgname = pkgmgr_name(info->inst);
	filename = pkgmgr_filename(info->inst);
	slave = pkgmgr_slave(pkgname);
	if (!slave) {
		ErrPrint("Slave is not exists\n");
		return -EINVAL;
	}

	param = g_variant_new("(ssss)", pkgname, filename, signal, part);
	if (!param) {
		ErrPrint("Failed to create param\n");
		return -EFAULT;
	}

	slave_push_command(slave, pkgname, filename, "script", param, NULL, NULL);
	return 0;
}

int script_handler_load(struct script_info *info, int is_pd)
{
	int ret;
	int w, h;

	if (!info->port)
		return -EINVAL;

	if (info->loaded > 0) {
		info->loaded++;
		return 0;
	}

	ret = fb_create_buffer(info->fb);
	if (ret < 0)
		return ret;

	info->ee = fb_canvas(info->fb);
	if (!info->ee) {
		ErrPrint("Failed to get canvas\n");
		fb_destroy_buffer(info->fb);
		return -EFAULT;
	}

	ecore_evas_data_set(info->ee, "script,info", info);

	evas_event_callback_add(script_handler_evas(info),
			EVAS_CALLBACK_RENDER_FLUSH_POST, render_post_cb, info->inst);

	if (info->port->load(info->port_data, script_handler_evas(info)) < 0) {
		ErrPrint("Failed to add new script object\n");
		evas_event_callback_del(script_handler_evas(info), EVAS_CALLBACK_RENDER_FLUSH_POST, render_post_cb);
		fb_destroy_buffer(info->fb);
		return -EFAULT;
	}

	fb_get_size(info->fb, &w, &h);
	ecore_evas_resize(info->ee, w, h);
	ecore_evas_show(info->ee);
	ecore_evas_activate(info->ee);
	fb_sync(info->fb);
	info->loaded = 1;

	script_signal_emit(script_handler_evas(info), pkgmgr_filename(info->inst), is_pd ? "pd,show" : "lb,show");
	return 0;
}

int script_handler_unload(struct script_info *info, int is_pd)
{
	Ecore_Evas *ee;

	if (!info->port)
		return -EINVAL;

	info->loaded--;
	if (info->loaded > 0)
		return 0;

	if (info->loaded < 0) {
		info->loaded = 0;
		return 0;
	}

	if (info->port->unload(info->port_data, script_handler_evas(info)) < 0)
		ErrPrint("Failed to unload script object. but go ahead\n");

	script_signal_emit(script_handler_evas(info), pkgmgr_filename(info->inst), is_pd ? "pd,hide" : "lb,hide");
	evas_event_callback_del(script_handler_evas(info), EVAS_CALLBACK_RENDER_FLUSH_POST, render_post_cb);

	ee = fb_canvas(info->fb);
	if (ee)
		ecore_evas_data_set(ee, "script,info", NULL);

	fb_destroy_buffer(info->fb);
	return 0;
}

struct script_info *script_handler_create(struct inst_info *inst, const char *file, const char *group, int w, int h)
{
	struct script_info *info;
	char *filename;
	int fname_len;

	fname_len = strlen(g_conf.path.image) + strlen(basename((char*)file)) + 30;

	filename = malloc(fname_len);
	if (!filename) {
		ErrPrint("Memory: %s\n", strerror(errno));
		return NULL;
	}

	snprintf(filename, fname_len, "%s%s.%lf",
			g_conf.path.image, basename((char*)file), util_get_timestamp());

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Memory: %s\n", strerror(errno));
		free(filename);
		return NULL;
	}

	info->fb = fb_create(filename, w, h);
	free(filename);
	if (!info->fb) {
		ErrPrint("Failed to create a FB (%dx%d)\n", w, h);
		free(info);
		return NULL;
	}

	info->inst = inst;
	info->ee = NULL;
	info->loaded = 0;
	info->port = find_port(pkgmgr_script(inst));
	if (!info->port) {
		fb_destroy(info->fb);
		free(info);
		return NULL;
	}

	info->port_data = info->port->create(file, group, w, h);
	if (!info->port_data) {
		fb_destroy(info->fb);
		free(info);
		return NULL;
	}

	return info;
}

int script_handler_destroy(struct script_info *info)
{
	if (!info->port)
		return -EINVAL;

	if (info->loaded != 0)
		return -EINVAL;

	if (info->port->destroy(info->port_data) < 0)
		ErrPrint("Failed to destroy port, but go ahead\n");

	fb_destroy(info->fb);
	free(info);
	return 0;
}

int script_handler_is_loaded(struct script_info *info)
{
	return info->loaded > 0;
}

struct fb_info *script_handler_fb(struct script_info *info)
{
	return info->fb;
}

void *script_handler_evas(struct script_info *info)
{
	return ecore_evas_get(info->ee);
}

static int update_script_text(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;

	if (!block || !block->part || !block->data)
		return -EINVAL;

	info = is_pd ? pkgmgr_pd_script(inst) : pkgmgr_lb_script(inst);
	if (!info)
		return -EFAULT;

	if (!info->port)
		return -EINVAL;

	info->port->update_text(info->port_data, script_handler_evas(info), block->id, block->part, block->data);
	return 0;
}

static int update_script_image(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;

	if (!block || !block->part)
		return -EINVAL;

	info = is_pd ? pkgmgr_pd_script(inst) : pkgmgr_lb_script(inst);
	if (!info)
		return -EFAULT;

	if (!info->port)
		return -EINVAL;

	info->port->update_image(info->port_data, script_handler_evas(info), block->id, block->part, block->data);
	return 0;
}

static int update_script_script(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;

	if (!block || !block->part)
		return -EINVAL;

	info = is_pd ? pkgmgr_pd_script(inst) : pkgmgr_lb_script(inst);
	if (!info)
		return -EFAULT;

	if (!info->port)
		return -EINVAL;

	info->port->update_script(info->port_data, script_handler_evas(info), block->id, block->part, block->data, block->group);
	return 0;
}

static int update_script_signal(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;

	if (!block)
		return -EINVAL;

	info = is_pd ? pkgmgr_pd_script(inst) : pkgmgr_lb_script(inst);
	if (!info)
		return -EFAULT;

	if (!info->port)
		return -EINVAL;

	info->port->update_signal(info->port_data, script_handler_evas(info), block->id, block->part, block->data);
	return 0;
}

static int update_script_drag(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	double dx, dy;

	if (!block || !block->data || !block->part)
		return -EINVAL;

	info = is_pd ? pkgmgr_pd_script(inst) : pkgmgr_lb_script(inst);
	if (!info)
		return -EFAULT;

	if (sscanf(block->data, "%lfx%lf", &dx, &dy) != 2) {
		ErrPrint("Invalid format of data\n");
		return -EINVAL;
	}

	if (!info->port)
		return -EINVAL;

	info->port->update_drag(info->port_data, script_handler_evas(info), block->id, block->part, dx, dy);
	return 0;
}

static int update_info(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;

	if (!block || !block->part || !block->data)
		return -EINVAL;

	info = is_pd ? pkgmgr_pd_script(inst) : pkgmgr_lb_script(inst);
	if (!info)
		return -EFAULT;

	if (!info->port)
		return -EINVAL;

	if (!strcasecmp(block->part, INFO_SIZE)) {
		Evas_Coord w, h;

		if (sscanf(block->data, "%dx%d", &w, &h) != 2) {
			ErrPrint("Invalid format (%s)\n", block->data);
			return -EINVAL;
		}

		info->port->update_size(info->port_data, script_handler_evas(info), block->id, w, h);
	} else if (!strcasecmp(block->part, INFO_CATEGORY)) {
		info->port->update_category(info->port_data, script_handler_evas(info), block->id, block->data);
	}

	return 0;
}

int script_handler_parse_desc(const char *pkgname, const char *filename, const char *descfile, int is_pd)
{
	struct inst_info *inst;
	FILE *fp;
	int ch;
	int lineno;
	enum state {
		UNKNOWN = 0x10,
		BLOCK_OPEN = 0x11,
		FIELD = 0x12,
		VALUE = 0x13,
		BLOCK_CLOSE = 0x14,

		VALUE_TYPE = 0x00,
		VALUE_PART = 0x01,
		VALUE_DATA = 0x02,
		VALUE_FILE = 0x03,
		VALUE_GROUP = 0x04,
		VALUE_ID = 0x05,
	};
	const char *field_name[] = {
		"type",
		"part",
		"data",
		"file",
		"group",
		"id",
		NULL
	};
	enum state state;
	register int field_idx;
	register int idx = 0;
	register int i;
	struct block *block;
	struct {
		const char *type;
		int (*handler)(struct inst_info *inst, struct block *block, int is_pd);
	} handlers[] = {
		{
			.type = TYPE_TEXT,
			.handler = update_script_text,
		},
		{
			.type = TYPE_IMAGE,
			.handler = update_script_image,
		},
		{
			.type = TYPE_EDJE,
			.handler = update_script_script,
		},
		{
			.type = TYPE_SIGNAL,
			.handler = update_script_signal,
		},
		{
			.type = TYPE_DRAG,
			.handler = update_script_drag,
		},
		{
			.type = TYPE_INFO,
			.handler = update_info,
		},
		{
			.type = NULL,
			.handler = NULL,
		},
	};

	block = NULL;
	inst = pkgmgr_find(pkgname, filename);
	if (!inst)
		return -EINVAL;

	fp = fopen(descfile, "rt");
	if (!fp) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
	}

	state = UNKNOWN;
	field_idx = 0;
	lineno = 1;

	block = NULL;
	while (!feof(fp)) {
		ch = getc(fp);
		if (ch == '\n')
			lineno++;

		switch (state) {
		case UNKNOWN:
			if (ch == '{') {
				state = BLOCK_OPEN;
				break;
			}

			if (!isspace(ch)) {
				fclose(fp);
				return -EINVAL;
			}
			break;

		case BLOCK_OPEN:
			if (isblank(ch))
				break;

			if (ch != '\n') {
				DbgPrint("Syntax error: New line must has to be started right after '{'\n");
				goto errout;
			}

			block = calloc(1, sizeof(*block));
			if (!block) {
				ErrPrint("Heap: %s\n", strerror(errno));
				fclose(fp);
				return -ENOMEM;
			}

			state = FIELD;
			idx = 0;
			field_idx = 0;
			break;

		case FIELD:
			if (isspace(ch))
				break;

			if (ch == '}') {
				state = BLOCK_CLOSE;
				break;
			}

			if (ch == '=') {
				if (field_name[field_idx][idx] != '\0') {
					DbgPrint("Syntax error: Unrecognized field\n");
					goto errout;
				}

				switch (field_idx) {
				case 0:
					state = VALUE_TYPE;
					if (block->type) {
						free(block->type);
						block->type = NULL;
						block->type_len = 0;
					}
					idx = 0;
					break;
				case 1:
					state = VALUE_PART;
					if (block->part) {
						free(block->part);
						block->part = NULL;
						block->part_len = 0;
					}
					idx = 0;
					break;
				case 2:
					state = VALUE_DATA;
					if (block->data) {
						free(block->data);
						block->data = NULL;
						block->data_len = 0;
					}
					idx = 0;
					break;
				case 3:
					state = VALUE_FILE;
					if (block->file) {
						free(block->file);
						block->file = NULL;
						block->file_len = 0;
					}
					idx = 0;
					break;
				case 4:
					state = VALUE_GROUP;
					if (block->group) {
						free(block->group);
						block->group = NULL;
						block->group_len = 0;
					}
					idx = 0;
					break;
				case 5:
					state = VALUE_ID;
					if (block->id) {
						free(block->id);
						block->id = NULL;
						block->id_len = 0;
					}
					idx = 0;
					break;
				default:
					DbgPrint("Syntax error: Unrecognized field\n");
					goto errout;
				}

				break;
			}

			if (ch == '\n')
				goto errout;

			if (field_name[field_idx][idx] != ch) {
				ungetc(ch, fp);
				if (ch == '\n')
					lineno--;

				while (--idx >= 0)
					ungetc(field_name[field_idx][idx], fp);

				field_idx++;
				if (field_name[field_idx] == NULL) {
					ErrPrint("Syntax error: Unrecognized field\n");
					goto errout;
				}

				idx = 0;
				break;
			}

			idx++;
			break;

		case VALUE_TYPE:
			if (idx == block->type_len) {
				block->type_len += 256;
				block->type =
					realloc(block->type, block->type_len);
				if (!block->type) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			if (ch == '\n') {
				block->type[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->type[idx] = ch;
			idx++;
			break;

		case VALUE_PART:
			if (idx == block->part_len) {
				block->part_len += 256;
				block->part =
					realloc(block->part, block->part_len);
				if (!block->part) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			if (ch == '\n') {
				block->part[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->part[idx] = ch;
			idx++;
			break;

		case VALUE_DATA:
			if (idx == block->data_len) {
				block->data_len += 256;
				block->data =
					realloc(block->data, block->data_len);
				if (!block->data) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			if (ch == '\n') {
				block->data[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->data[idx] = ch;
			idx++;
			break;

		case VALUE_FILE:
			if (idx == block->file_len) {
				block->file_len += 256;
				block->file =
					realloc(block->file, block->file_len);
				if (!block->file) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			if (ch == '\n') {
				block->file[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->file[idx] = ch;
			idx++;
			break;

		case VALUE_GROUP:
			if (idx == block->group_len) {
				block->group_len += 256;
				block->group = realloc(block->group,
							block->group_len);
				if (!block->group) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			if (ch == '\n') {
				block->group[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->group[idx] = ch;
			idx++;
			break;
		case VALUE_ID:
			if (idx == block->id_len) {
				block->id_len += 256;
				block->id = realloc(block->id, block->id_len);
				if (!block->id) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			if (ch == '\n') {
				block->id[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->id[idx] = ch;
			idx++;
			break;
		case BLOCK_CLOSE:
			if (!block->file) {
				block->file = strdup(filename);
				if (!block->file) {
					ErrPrint("Failed to allocate memory\n");
					goto errout;
				}
			}

			i = 0;
			while (handlers[i].type) {
				if (!strcasecmp(handlers[i].type, block->type)) {
					handlers[i].handler(inst, block, is_pd);
					break;
				}
				i++;
			}

			if (!handlers[i].type)
				ErrPrint("Unknown block type: %s\n", block->type);

			free(block->file);
			free(block->type);
			free(block->part);
			free(block->data);
			free(block->group);
			free(block->id);
			free(block);

			state = UNKNOWN;
			break;

		default:
			break;
		} /* switch */
	} /* while */

	if (state != UNKNOWN) {
		ErrPrint("Unknown state\n");
		goto errout;
	}

	fclose(fp);
	return 0;

errout:
	ErrPrint("Parse error at %d file %s\n", lineno, descfile);
	if (block) {
		free(block->file);
		free(block->type);
		free(block->part);
		free(block->data);
		free(block->group);
		free(block->id);
		free(block);
	}
	fclose(fp);
	return -EINVAL;
}

int script_init(void)
{
	struct script_port *item;
	struct dirent *ent;
	DIR *dir;
	char *path;
	int pathlen;

	dir = opendir(g_conf.path.script_port);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;

		pathlen = strlen(ent->d_name) + strlen(g_conf.path.script_port) + 1;
		path = malloc(pathlen);
		if (!path) {
			ErrPrint("Error: %s\n", strerror(errno));
			closedir(dir);
			return -ENOMEM;
		}

		snprintf(path, pathlen, "%s%s", g_conf.path.script_port, ent->d_name);

		item = malloc(sizeof(*item));
		if (!item) {
			ErrPrint("Error: %s\n", strerror(errno));
			free(path);
			closedir(dir);
			return -ENOMEM;
		}

		DbgPrint("Open SCRIPT PORT: %s\n", path);
		item->handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
		free(path);

		if (!item->handle) {
			ErrPrint("Error: %s\n", dlerror());
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->magic_id = dlsym(item->handle, "script_magic_id");
		if (!item->magic_id) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}
		DbgPrint("SCRIPT PORT magic id: %s [%p]\n", item->magic_id());

		item->update_text = dlsym(item->handle, "script_update_text");
		if (!item->update_text) {
			ErrPrint("Error: %s)\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->update_image = dlsym(item->handle, "script_update_image");
		if (!item->update_image) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->update_script = dlsym(item->handle, "script_update_script");
		if (!item->update_script) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->update_signal = dlsym(item->handle, "script_update_signal");
		if (!item->update_signal) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->update_drag = dlsym(item->handle, "script_update_drag");
		if (!item->update_drag) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->update_size = dlsym(item->handle, "script_update_size");
		if (!item->update_size) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->update_category = dlsym(item->handle, "script_update_category");
		if (!item->update_category) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->create = dlsym(item->handle, "script_create");
		if (!item->create) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->destroy = dlsym(item->handle, "script_destroy");
		if (!item->destroy) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->load = dlsym(item->handle, "script_load");
		if (!item->load) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->unload = dlsym(item->handle, "script_unload");
		if (!item->unload) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->init = dlsym(item->handle, "script_init");
		if (!item->init) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		item->fini = dlsym(item->handle, "script_fini");
		if (!item->fini) {
			ErrPrint("Error: %s\n", dlerror());
			dlclose(item->handle);
			free(item);
			closedir(dir);
			return -EFAULT;
		}

		s_info.script_port_list = eina_list_append(s_info.script_port_list, item);

		item->init();
	}

	closedir(dir);
	return 0;
}

int script_fini(void)
{
	struct script_port *item;
	/*!
	 * \TODO: Release all handles
	 */
	EINA_LIST_FREE(s_info.script_port_list, item) {
		item->fini();
		dlclose(item->handle);
		free(item);
	}

	return 0;
}

/* End of a file */
