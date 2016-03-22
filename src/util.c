/*
 * Copyright 2016  Samsung Electronics Co., Ltd
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

#include "util.h"
#include "debug.h"
#include "conf.h"

#define DELIM ';'

int errno;

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

HAPI void util_setup_log_disk(void)
{
	if (access(CONF_LOG_PATH, R_OK | W_OK | X_OK) == 0) {
		DbgPrint("[%s] is already accessible\n", CONF_LOG_PATH);
		return;
	}

	DbgPrint("Initiate the critical log folder [%s]\n", CONF_LOG_PATH);
	if (mkdir(CONF_LOG_PATH, 0755) < 0)
		ErrPrint("mkdir: %d\n", errno);
}

HAPI const char *util_basename(const char *name)
{
	int length;
	length = name ? strlen(name) : 0;
	if (!length)
		return ".";

	while (--length > 0 && name[length] != '/')
		;

	return length <= 0 ? name : (name + length + (name[length] == '/'));
}

/* End of a file */
