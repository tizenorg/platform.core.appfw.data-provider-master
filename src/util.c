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
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <strings.h>

#include <sys/smack.h>
#include <dlog.h>
#include <Eina.h>
#include <Ecore.h>
#if defined(HAVE_LIVEBOX)
#include <widget_errno.h>
#include <widget_conf.h>
#else
#include "lite-errno.h"
#define WIDGET_CONF_IMAGE_PATH "/tmp/"
#endif

#include "util.h"
#include "debug.h"
#include "conf.h"

#define DELIM ';'

int errno;

HAPI unsigned long util_string_hash(const char *str)
{
	unsigned long ret = 0;

	while (*str) {
		ret += (unsigned long)(*str++);
	}

	ret %= 371773;
	return ret;
}

HAPI double util_timestamp(void)
{
#if defined(_USE_ECORE_TIME_GET)
	return ecore_time_get();
#else
	struct timeval tv;
	if (gettimeofday(&tv, NULL) < 0) {
		static unsigned long internal_count = 0;
		ErrPrint("gettimeofday: %d\n", errno);
		tv.tv_sec = internal_count++;
		tv.tv_usec = 0;
	}

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
#endif
}

HAPI int util_check_ext(const char *filename, const char *check_ptr)
{
	int name_len;

	name_len = strlen(filename);
	while (--name_len >= 0 && *check_ptr) {
		if (filename[name_len] != *check_ptr) {
			return WIDGET_ERROR_INVALID_PARAMETER;
		}

		check_ptr ++;
	}

	return WIDGET_ERROR_NONE;
}

HAPI int util_unlink(const char *filename)
{
	char *descfile;
	int desclen;
	int ret;

	if (!filename) {
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	desclen = strlen(filename) + 6; /* .desc */
	descfile = malloc(desclen);
	if (!descfile) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	ret = snprintf(descfile, desclen, "%s.desc", filename);
	if (ret < 0) {
		ErrPrint("snprintf: %d\n", errno);
		DbgFree(descfile);
		return WIDGET_ERROR_FAULT;
	}

	(void)unlink(descfile);
	DbgFree(descfile);
	(void)unlink(filename);

	return WIDGET_ERROR_NONE;
}

HAPI char *util_slavename(void)
{
	char slavename[BUFSIZ];
	static unsigned long idx = 0;

	snprintf(slavename, sizeof(slavename), "%lu_%lf", idx++, util_timestamp());
	return strdup(slavename);
}

/*!
 * Return size of stroage in Bytes unit.
 */
HAPI unsigned long long util_free_space(const char *path)
{
	struct statvfs st;
	unsigned long long space;

	if (statvfs(path, &st) < 0) {
		ErrPrint("statvfs: %d\n", errno);
		return 0lu;
	}

	space = (unsigned long long)st.f_bsize * (unsigned long long)st.f_bavail;
	DbgPrint("Available size: %llu, f_bsize: %lu, f_bavail: %lu\n", space, st.f_bsize, st.f_bavail);
	/*!
	 * \note
	 * Must have to check the overflow
	 */

	return space;
}

static inline char *extend_heap(char *buffer, int *sz, int incsz)
{
	char *tmp;

	*sz += incsz;
	tmp = realloc(buffer, *sz);
	if (!tmp) {
		ErrPrint("realloc: %d\n", errno);
		return NULL;
	}

	return tmp;
}

HAPI double util_time_delay_for_compensation(double period)
{
	unsigned long long curtime;
	unsigned long long _period;
	unsigned long long remain;
	struct timeval tv;
	double ret;

	if (period == 0.0f) {
		DbgPrint("Period is ZERO\n");
		return 0.0f;
	}

	if (gettimeofday(&tv, NULL) < 0){
		ErrPrint("gettimeofday: %d\n", errno);
		return period;
	}

	curtime = (unsigned long long)tv.tv_sec * 1000000llu + (unsigned long long)tv.tv_usec;

	_period = (unsigned long long)(period * (double)1000000);
	if (_period == 0llu) {
		ErrPrint("%lf <> %llu\n", period, _period);
		return period;
	}

	remain = curtime % _period;

	ret = (double)remain / (double)1000000;
	return period - ret;
}

HAPI void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data)
{
	Ecore_Timer *timer;
	double delay;

	timer = ecore_timer_add(interval, cb, data);
	if (!timer) {
		return NULL;
	}

	delay = util_time_delay_for_compensation(interval) - interval;
	ecore_timer_delay(timer, delay);
	DbgPrint("Compensate timer: %lf\n", delay);

	return timer;
}

HAPI void util_timer_interval_set(void *timer, double interval)
{
	double delay;
	ecore_timer_interval_set(timer, interval);

	delay = util_time_delay_for_compensation(interval) - interval;
	ecore_timer_delay(timer, delay);
}

HAPI int util_unlink_files(const char *folder)
{
	struct stat info;
	DIR *handle;
	struct dirent *entry;
	char *abspath;
	int len;

	if (lstat(folder, &info) < 0) {
		ErrPrint("lstat: %d\n", errno);
		return WIDGET_ERROR_IO_ERROR;
	}

	if (!S_ISDIR(info.st_mode)) {
		ErrPrint("Error: %s is not a folder", folder);
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	handle = opendir(folder);
	if (!handle) {
		ErrPrint("opendir: %d\n", errno);
		return WIDGET_ERROR_IO_ERROR;
	}

	while ((entry = readdir(handle))) {
		if (!strcmp(entry->d_name, ".")) {
			continue;
		}

		if (!strcmp(entry->d_name, "..")) {
			continue;
		}

		len = strlen(folder) + strlen(entry->d_name) + 3;
		abspath = calloc(1, len);
		if (!abspath) {
			ErrPrint("calloc: %d\n", errno);
			continue;
		}
		snprintf(abspath, len - 1, "%s/%s", folder, entry->d_name);

		if (unlink(abspath) < 0) {
			DbgPrint("unlink: %d\n", errno);
		}

		DbgFree(abspath);
	}

	if (closedir(handle) < 0) {
		ErrPrint("closedir: %d\n", errno);
	}
	return WIDGET_ERROR_NONE;
}

HAPI void util_setup_log_disk(void)
{
	int ret;

	if (access(WIDGET_CONF_LOG_PATH, R_OK | W_OK | X_OK) == 0) {
		DbgPrint("[%s] is already accessible\n", WIDGET_CONF_LOG_PATH);
		return;
	}

	DbgPrint("Initiate the critical log folder [%s]\n", WIDGET_CONF_LOG_PATH);
	if (mkdir(WIDGET_CONF_LOG_PATH, 0755) < 0) {
		ErrPrint("mkdir: %d\n", errno);
	} else {
		if (chmod(WIDGET_CONF_LOG_PATH, 0750) < 0) {
			ErrPrint("chmod: %d\n", errno);
		}

		if (chown(WIDGET_CONF_LOG_PATH, 5000, 5000) < 0) {
			ErrPrint("chown: %d\n", errno);
		}

		ret = smack_setlabel(WIDGET_CONF_LOG_PATH, DATA_SHARE_LABEL, SMACK_LABEL_ACCESS);
		if (ret != 0) {
			ErrPrint("Failed to set SMACK for %s (%d)\n", WIDGET_CONF_LOG_PATH, ret);
		} else {
			ret = smack_setlabel(WIDGET_CONF_LOG_PATH, "1", SMACK_LABEL_TRANSMUTE);
			DbgPrint("[%s] is successfully created (t: %d)\n", WIDGET_CONF_LOG_PATH, ret);
		}
	}
}

HAPI int util_service_is_enabled(const char *tag)
{
	return !!strcasestr(WIDGET_CONF_SERVICES, tag);
}

HAPI int util_string_is_in_list(const char *str, const char *haystack)
{
	int len;
	const char *ptr;
	const char *check;

	if (!str) {
		return 0;
	}

	check = ptr = haystack;
	len = 0;
	while (*ptr) {
		switch (*ptr++) {
		case DELIM:
			if (len > 0) {
				if (!strncasecmp(str, check, len) && str[len] == '\0') {
					return 1;
				}
			}
			check = ptr;
			len = 0;
			break;
		default:
			len++;
			break;
		}
	}

	return (len > 0 && !strncmp(str, check, len) && str[len] == '\0');
}

/* End of a file */
