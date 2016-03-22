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

extern double util_timestamp(void);
extern void util_setup_log_disk(void);
extern const char *util_basename(const char *name);

#define SCHEMA_FILE	"file://"
#define SCHEMA_PIXMAP	"pixmap://"
#define SCHEMA_SHM	"shm://"

#define CRITICAL_SECTION_BEGIN(handle) \
do { \
	int ret; \
	ret = pthread_mutex_lock(handle); \
	if (ret != 0) \
		ErrPrint("pthread_mutex_lock: %d\n", ret); \
} while (0)

#define CRITICAL_SECTION_END(handle) \
do { \
	int ret; \
	ret = pthread_mutex_unlock(handle); \
	if (ret != 0) \
		ErrPrint("pthread_mutex_unlock: %d\n", ret); \
} while (0)

#define CANCEL_SECTION_BEGIN() do { \
	int ret; \
	ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); \
	if (ret != 0) \
		ErrPrint("Unable to set cancelate state: %d\n", ret); \
} while (0)

#define CANCEL_SECTION_END() do { \
	int ret; \
	ret = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL); \
	if (ret != 0) \
		ErrPrint("Unable to set cancelate state: %d\n", ret); \
} while (0)

#define CLOSE_PIPE(p)	do { \
	int status; \
	status = close(p[PIPE_READ]); \
	if (status < 0) \
		ErrPrint("close: %d\n", errno); \
	status = close(p[PIPE_WRITE]); \
	if (status < 0) \
		ErrPrint("close: %d\n", errno); \
} while (0)

#define PIPE_READ 0
#define PIPE_WRITE 1
#define PIPE_MAX 2

/* End of a file */
