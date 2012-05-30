/*
 * com.samsung.live-data-provider
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: Sung-jae Park <nicesj.park@samsung.com>, Youngjoo Park <yjoo93.park@samsung.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h> /* strtod */
#include <errno.h>
#include <libgen.h> /* basename */
#include <ctype.h> /* isspace */

#include <Eina.h>

#include <dlog.h>

#include "debug.h"
#include "conf.h"
#include "parser.h"

static Eina_List *s_list;
int errno;

struct item {
	char *filename;
	double period;
	int timeout;
	int network;
	int auto_launch;
	unsigned int size;
	unsigned int pd_width;
	unsigned int pd_height;
	char *group;
	int secured;

	char *pd_path;
	char *pd_group;

	char *lb_path;
	char *lb_group;
	int pinup;
	int text_pd;
	int text_lb;

	char *abi;

	char *script;
};

double parser_period(struct item *handle)
{
	return handle->period;
}

int parser_network(struct item *handle)
{
	return handle->network;
}

int parser_timeout(struct item *handle)
{
	return handle->timeout;
}

int parser_auto_launch(struct item *handle)
{
	return handle->auto_launch;
}

const char *parser_script(struct item *handle)
{
	return handle->script;
}

const char *parser_abi(struct item *handle)
{
	return handle->abi;
}

unsigned int parser_size(struct item *handle)
{
	return handle->size;
}

const char *parser_lb_path(struct item *handle)
{
	return handle->lb_path;
}

const char *parser_lb_group(struct item *handle)
{
	return handle->lb_group;
}

const char *parser_pd_path(struct item *handle)
{
	return handle->pd_path;
}

const char *parser_pd_group(struct item *handle)
{
	return handle->pd_group;
}

const char *parser_group_str(struct item *handle)
{
	return handle->group;
}

int parser_secured(struct item *handle)
{
	return handle->secured;
}

void parser_get_pdsize(struct item *handle, unsigned int *width, unsigned int *height)
{
	*width = handle->pd_width;
	*height = handle->pd_height;
}

int parser_pinup(struct item *handle)
{
	return handle->pinup;
}

int parser_text_lb(struct item *handle)
{
	return handle->text_lb;
}

int parser_text_pd(struct item *handle)
{
	return handle->text_pd;
}

int parser_find(const char *pkgname)
{
	Eina_List *l;
	struct item *item;
	char *filename;
	int len;
	int ret;

	len = strlen(pkgname) * 2 + strlen(g_conf.path.conf);

	filename = malloc(len);
	if (!filename)
		return 0;

	ret = snprintf(filename, len, g_conf.path.conf, pkgname, pkgname);
	if (ret < 0) {
		free(filename);
		return -EFAULT;
	}

	LOGD("Conf file is %s for package %s\n", filename, pkgname);

	EINA_LIST_FOREACH(s_list, l, item) {
		if (!strcmp(item->filename, filename)) {
			free(filename);
			return (int)item;
		}
	}

	free(filename);
	return 0;
}

static inline int parse_size(const char *buffer, unsigned int *size)
{
	register int i;
	register int j;
	int w;
	int h;

	enum {
		START,
		WIDTH,
		DELIM,
		HEIGHT,
		ERROR,
		STOP,
		END,
	} state;

	*size = 0;
	state = START;
	i = 0;
	w = 0;
	h = 0;

	while (state != ERROR && state != END) {
		switch (state) {
		case START:
			switch (buffer[i]) {
			case '1'...'9':
				state = WIDTH;
				break;
			case ' ':
			case '\t':
			case ';':
				i++;
				break;
			case '\0':
				state = END;
				break;
			default:
				state = ERROR;
				break;
			}
			break;
		case WIDTH:
			switch (buffer[i]) {
			case '0'...'9':
				w = (w * 10) + (buffer[i] - '0');
				i++;
				break;
			case 'x':
				state = DELIM;
				i++;
				break;
			default:
				state = ERROR;
				break;
			}

			break;
		case DELIM:
			switch (buffer[i]) {
			case '1'...'9':
				state = HEIGHT;
				break;
			case ' ':
			case '\t':
				i++;
				break;
			default:
				state = ERROR;
				break;
			}
			break;
		case HEIGHT:
			switch (buffer[i]) {
			case '0'...'9':
				h = (h * 10) + (buffer[i] - '0');
				i++;
				break;
			case ';':
			case '\0':
				state = STOP;
				break;
			default:
				state = ERROR;
				break;
			}
			break;
		case STOP:
			for (j = 0; j < g_conf.max_size_type; j++) {
				w = (double)w * g_conf.width / BASE_W;
				h = (double)h * g_conf.height / BASE_H;
				if (g_conf.size[j].width == w &&
						g_conf.size[j].height == h) {
					*size |= (0x01 << j);
					break;
				}
			}

			if (buffer[i] == ';')
				state = START;
			else if (buffer[i] == '\0')
				state = END;

			w = 0;
			h = 0;
			break;
		case ERROR:
		default:
			return -1;
		}
	}

	return *size ? 0 : -1;
}

static inline char *dup_rtrim(char *buffer)
{
	int len;
	char *ret;

	len = strlen(buffer);
	while (len > 0 && isspace(buffer[len - 1]))
		len--;

	if (len <= 0)
		return NULL;

	buffer[len] = '\0';

	ret = malloc(len + 1);
	if (!ret)
		return NULL;

	strcpy(ret, buffer);
	return ret;
}

static void period_handler(struct item *item, char *buffer)
{
	char *tmp = NULL;
	item->period = strtod(buffer, &tmp);
}

static void timeout_handler(struct item *item, char *buffer)
{
	item->timeout = atoi(buffer);
}

static void network_handler(struct item *item, char *buffer)
{
	item->network = atoi(buffer);
}

static void auto_launch_handler(struct item *item, char *buffer)
{
	item->auto_launch = atoi(buffer);
}

static void size_handler(struct item *item, char *buffer)
{
	if (parse_size(buffer, &item->size) == -1) {
		LOGE("Failed to get size\n");
		item->size = 0x00000001;
	}
}

static void pd_size_handler(struct item *item, char *buffer)
{
	if (sscanf(buffer, "%ux%u", &item->pd_width, &item->pd_height) != 2)
		LOGE("parse pd size\n");
}

static void text_lb_handler(struct item *item, char *buffer)
{
	item->text_lb = !!atoi(buffer);
}

static void abi_handler(struct item *item, char *buffer)
{
	item->abi = dup_rtrim(buffer);
}

static void script_handler(struct item *item, char *buffer)
{
	item->script = dup_rtrim(buffer);
}

static void text_pd_handler(struct item *item, char *buffer)
{
	item->text_pd = !!atoi(buffer);
}

static void pinup_handler(struct item *item, char *buffer)
{
	item->pinup = !!atoi(buffer);
}

static void lb_path_handler(struct item *item, char *buffer)
{
	if (item->lb_path)
		free(item->lb_path);

	item->lb_path = dup_rtrim(buffer);
	if (!item->lb_path)
		LOGD("Error: %s\n", strerror(errno));
}

static void group_handler(struct item *item, char *buffer)
{
	if (item->group)
		free(item->group);

	item->group = dup_rtrim(buffer);
	if (!item->group)
		LOGD("Error: %s\n", strerror(errno));
}

static void secured_handler(struct item *item, char *buffer)
{
	item->secured = atoi(buffer);
}

static void lb_group_handler(struct item *item, char *buffer)
{
	if (item->lb_group)
		free(item->lb_group);

	item->lb_group = dup_rtrim(buffer);
	if (!item->lb_group)
		LOGD("Error: %s\n", strerror(errno));
}

static void pd_path_handler(struct item *item, char *buffer)
{
	if (item->pd_path)
		free(item->pd_path);

	item->pd_path = dup_rtrim(buffer);
	if (!item->pd_path)
		LOGD("Error: %s\n", strerror(errno));
}

static void pd_group_handler(struct item *item, char *buffer)
{
	if (item->pd_group)
		free(item->pd_group);

	item->pd_group = dup_rtrim(buffer);
	if (!item->pd_group)
		LOGD("Error: %s\n", strerror(errno));
}

struct item *parser_load(const char *pkgname)
{
	struct item *item;
	FILE *fp;
	int c;
	enum state {
		START,
		SPACE,
		TOKEN,
		VALUE,
		ERROR,
		COMMENT,
		END,
	} state;
	int ch_idx;
	int token_idx;
	int buffer_idx;
	int quote;
	int len;
	int linelen;
	char buffer[256];
	static const struct token_parser {
		const char *name;
		void (*handler)(struct item *, char *buffer);
	} token_handler[] = {
		{
			.name = "period",
			.handler = period_handler,
		},
		{
			.name = "timeout",
			.handler = timeout_handler,
		},
		{
			.name = "network",
			.handler = network_handler,
		},
		{
			.name = "auto_launch",
			.handler = auto_launch_handler,
		},
		{
			.name = "size",
			.handler = size_handler,
		},
		{
			.name = "group",
			.handler = group_handler,
		},
		{
			.name = "secured",
			.handler = secured_handler,
		},
		{
			.name = "livebox_path",
			.handler = lb_path_handler,
		},
		{
			.name = "livebox_group",
			.handler = lb_group_handler,
		},
		{
			.name = "pd_path",
			.handler = pd_path_handler,
		},
		{
			.name = "pd_group",
			.handler = pd_group_handler,
		},
		{
			.name = "pd_size",
			.handler = pd_size_handler,
		},
		{
			.name = "pinup",
			.handler = pinup_handler,
		},
		{
			.name = "text_livebox",
			.handler = text_lb_handler,
		},
		{
			.name = "text_pd",
			.handler = text_pd_handler,
		},
		{
			.name = "script",
			.handler = script_handler,
		},
		{
			.name = "abi",
			.handler = abi_handler,
		},
		{
			.name = NULL,
			.handler = NULL,
		},
	};
	int ret;

	item = calloc(1, sizeof(*item));
	if (!item)
		return 0;

	/* live-, .conf */
	len = strlen(g_conf.path.conf) + strlen(pkgname) * 2;
	item->filename = malloc(len);
	if (!item->filename) {
		LOGD("Error: %s\n", strerror(errno));
		free(item);
		return 0;
	}

	ret = snprintf(item->filename, len, g_conf.path.conf, pkgname, pkgname);
	if (ret < 0) {
		LOGD("Error: %s\n", strerror(errno));
		free(item->filename);
		free(item);
		return 0;
	}

	item->lb_path = NULL;
	item->lb_group = NULL;
	item->pd_width = 0;
	item->pd_height = 0;
	item->auto_launch = 1;
	item->size = 0x00000001;
	item->group = NULL;
	item->secured = 0;
	item->pinup = 1;

	fp = fopen(item->filename, "rt");
	if (!fp) {
		free(item->filename);
		free(item);
		return 0;
	}

	state = START;
	ch_idx = 0;
	token_idx = -1;
	buffer_idx = 0;
	quote = 0;
	linelen = 0;
	do {
		c = getc(fp);
		if ((c == EOF) && (state == VALUE)) {
			LOGD("[%s:%d] VALUE state EOF\n", __func__, __LINE__);
			state = END;
		}

		switch (state) {
		case COMMENT:
			if (c == CR || c == LF) {
				buffer[buffer_idx] = '\0';

				state = START;
				token_idx = -1;
				ch_idx = 0;
				buffer_idx = 0;
				linelen = -1; /* Will be ZERO by follwing increment code */
				quote = 0;
			} else {
				buffer[buffer_idx++] = c;
				if (buffer_idx == (sizeof(buffer) - 1)) {
					buffer[buffer_idx] = '\0';
					buffer_idx = 0;
				}
			}
			break;
		case START:
			if (linelen == 0 && c == '#') {
				state = COMMENT;
			} else if (isspace(c)) {
				/* Ignore empty space */
			} else {
				state = TOKEN;
				ungetc(c, fp);
			}
			break;
		case SPACE:
			if (c == '=')
				state = VALUE;
			else if (!isspace(c))
				state = ERROR;
			break;
		case VALUE:
			if (c == '"') {
				if (quote == 1) {
					buffer[buffer_idx] = '\0';
					state = END;
				} else if (buffer_idx != 0) {
					buffer[buffer_idx++] = c;
					if (buffer_idx >= sizeof(buffer))
						state = ERROR;
				} else {
					quote = 1;
				}
			} else if (isspace(c)) {
				if (buffer_idx == 0) {
					/* Ignore */
				} else if (quote == 1) {
					buffer[buffer_idx++] = c;
					if (buffer_idx >= sizeof(buffer))
						state = ERROR;
				} else {
					buffer[buffer_idx] = '\0';
					ungetc(c, fp);
					state = END;
				}
			} else {
				buffer[buffer_idx++] = c;
				if (buffer_idx >= sizeof(buffer))
					state = ERROR;
			}
			break;
		case TOKEN:
			if (c == '=') {
				if (token_idx < 0)
					state = ERROR;
				else
					state = VALUE;
			} else if (isspace(c)) {
				if (token_idx < 0)
					break;

				if (token_handler[token_idx].name[ch_idx] != '\0')
					state = ERROR;
				else
					state = SPACE;
			} else  {
				if (token_idx < 0) {
					/* Now start to find a token! */
					token_idx = 0;
				}

				if (token_handler[token_idx].name[ch_idx] == c) {
					ch_idx++;
				} else {
					ungetc(c, fp);
					while (ch_idx-- > 0)
						ungetc(token_handler[token_idx].name[ch_idx], fp);

					token_idx++;

					if (token_handler[token_idx].name == NULL)
						state = ERROR;
					else
						ch_idx = 0;
				}
			}
			break;
		case ERROR:
			if (c == CR || c == LF) {
				state = START;
				token_idx = -1;
				buffer_idx = 0;
				ch_idx = 0;
				linelen = -1;
				quote = 0;
			}
			break;
		case END:
			if (c == LF || c == CR || c == EOF) {
				state = START;

				if (token_idx >= 0 && token_handler[token_idx].handler)
					token_handler[token_idx].handler(item, buffer);

				token_idx = -1;
				ch_idx = 0;
				buffer_idx = 0;
				linelen = -1;
				quote = 0;
				/* Finish */
			} else if (isspace(c)) {
				/* ignore */
			} else {
				state = ERROR;
			}
			break;
		default:
			/* ?? */
			break;
		}

		linelen++;
	 } while (c != EOF);

	fclose(fp);

	s_list = eina_list_append(s_list, item);
	return item;
}

int parser_unload(struct item *item)
{
	s_list = eina_list_remove(s_list, item);

	free(item->abi);
	free(item->script);
	free(item->group);
	free(item->pd_group);
	free(item->pd_path);
	free(item->lb_group);
	free(item->lb_path);
	free(item->filename);
	free(item);
	return 0;
}

/* End of a file */
