#include <stdio.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#include <dlog.h>

#include "util.h"
#include "debug.h"
#include "conf.h"

int errno;

HAPI unsigned long util_string_hash(const char *str)
{
	unsigned long ret = 0;

	while (*str)
		ret += (unsigned long)(*str++);

	ret %= 371773;
	return ret;
}

HAPI double util_timestamp(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) < 0) {
		static unsigned long internal_count = 0;
		ErrPrint("failed to get time of day: %s\n", strerror(errno));
		tv.tv_sec = internal_count++;
		tv.tv_usec = 0;
	}

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
}

HAPI int util_check_ext(const char *filename, const char *check_ptr)
{
	int name_len;

	name_len = strlen(filename);
	while (--name_len >= 0 && *check_ptr) {
		if (filename[name_len] != *check_ptr)
			return -EINVAL;

		check_ptr ++;
	}

	return 0;
}

static inline int check_native_livebox(const char *pkgname)
{
	int len;
	char *path;

	len = strlen(pkgname) * 2;
	len += strlen(ROOT_PATH);
	len += strlen("%s/libexec/liblive-%s.so");

	path = malloc(len + 1);
	if (!path) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	snprintf(path, len, "%s%s/libexec/liblive-%s.so", ROOT_PATH, pkgname, pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		DbgFree(path);
		return -EINVAL;
	}

	DbgFree(path);
	return 0;
}

static inline int check_web_livebox(const char *pkgname)
{
	int len;
	char *path;

	len = strlen(pkgname) * 2;
	len += strlen("/opt/usr/apps/%s/res/wgt/livebox/index.html");

	path = malloc(len + 1);
	if (!path) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	snprintf(path, len, "/opt/usr/apps/%s/res/wgt/livebox/index.html", pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		DbgFree(path);
		return -EINVAL;
	}

	DbgFree(path);
	return 0;
}

HAPI int util_validate_livebox_package(const char *pkgname)
{
	if (!pkgname) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	if (!check_native_livebox(pkgname) || !check_web_livebox(pkgname))
		return 0;

	return -EINVAL;
}

HAPI int util_unlink(const char *filename)
{
	char *descfile;
	int desclen;
	int ret;

	desclen = strlen(filename) + 6; /* .desc */
	descfile = malloc(desclen);
	if (!descfile) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	ret = snprintf(descfile, desclen, "%s.desc", filename);
	if (ret < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(descfile);
		return -EFAULT;
	}

	(void)unlink(descfile);
	DbgFree(descfile);
	(void)unlink(filename);

	return 0;
}

HAPI char *util_slavename(void)
{
	char slavename[BUFSIZ];
	static unsigned long idx = 0;

	snprintf(slavename, sizeof(slavename), "%lu_%lf", idx++, util_timestamp());
	return strdup(slavename);
}

HAPI const char *util_basename(const char *name)
{
	int length;
	length = name ? strlen(name) : 0;
	if (!length)
		return ".";

	while (--length > 0 && name[length] != '/');

	return length <= 0 ? name : (name + length + (name[length] == '/'));
}

HAPI unsigned long util_free_space(const char *path)
{
	struct statvfs st;
	unsigned long space;

	if (statvfs(path, &st) < 0) {
		ErrPrint("statvfs: %s\n", strerror(errno));
		return 0lu;
	}

	space = st.f_bsize * st.f_bfree;
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
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	return tmp;
}

HAPI char *util_replace_string(const char *src, const char *pattern, const char *replace)
{
	char *ret;
	int src_idx;
	int pattern_idx;
	int src_rollback_idx;
	int ret_rollback_idx;
	int ret_idx;
	int target_idx;
	int bufsz;
	int incsz;
	int matched;

	bufsz = strlen(src);
	incsz = bufsz;
	ret = malloc(bufsz + 1);
	if (!ret) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	pattern_idx = 0;
	ret_idx = 0;
	matched = 0;
	for (src_idx = 0; src[src_idx]; src_idx++) {
		if (!pattern[pattern_idx]) {
			while (replace[target_idx]) {
				ret[ret_idx] = replace[target_idx];
				ret_idx++;
				target_idx++;

				if (ret_idx >= bufsz) {
					char *tmp;

					tmp = extend_heap(ret, &bufsz, incsz);
					if (!tmp) {
						ErrPrint("Heap: %s\n", strerror(errno));
						DbgFree(ret);
						return NULL;
					}
					ret = tmp;
				}
			}

			pattern_idx = 0;
			src--;
			matched++;
			continue;
		} else if (src[src_idx] == pattern[pattern_idx]) {
			if (pattern_idx == 0) {
				src_rollback_idx = src_idx;
				ret_rollback_idx = ret_idx;
				target_idx = 0;
			}

			if (replace[target_idx]) {
				ret[ret_idx] = replace[target_idx];
				ret_idx++;
				target_idx++;
				if (ret_idx >= bufsz) {
					char *tmp;

					tmp = extend_heap(ret, &bufsz, incsz);
					if (!tmp) {
						ErrPrint("Heap: %s\n", strerror(errno));
						DbgFree(ret);
						return NULL;
					}
					ret = tmp;
				}
			}

			pattern_idx++;
			continue;
		} else if (pattern_idx > 0) {
			src_idx = src_rollback_idx;
			ret_idx = ret_rollback_idx;
			pattern_idx = 0;
		}

		ret[ret_idx] = src[src_idx];
		ret_idx++;
		if (ret_idx >= bufsz) {
			char *tmp;

			tmp = extend_heap(ret, &bufsz, incsz);
			if (!tmp) {
				ErrPrint("Heap: %s\n", strerror(errno));
				DbgFree(ret);
				return NULL;
			}
			ret = tmp;
		}
	}
	if (matched) {
		ret[ret_idx] = '\0';
	} else {
		DbgFree(ret);
		ret = NULL;
	}
	return ret;
}

HAPI const char *util_uri_to_path(const char *uri)
{
	int len;

	len = strlen(SCHEMA_FILE);
	if (strncasecmp(uri, SCHEMA_FILE, len))
		return NULL;

	return uri + len;
}

/* End of a file */
