#include <stdio.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <stdlib.h>

#include <dlog.h>

#include "util.h"
#include "debug.h"
#include "conf.h"

int errno;

unsigned long util_string_hash(const char *str)
{
	unsigned long ret = 0;

	while (*str)
		ret += (unsigned long)(*str++);

	ret %= 371773;
	return ret;
}

double util_timestamp(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
}

int util_check_ext(const char *filename, const char *check_ptr)
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

int util_validate_livebox_package(const char *pkgname)
{
	int len;
	char *path;

	if (!pkgname)
		return -EINVAL;

	len = strlen(pkgname) * 2;
	len += strlen(g_conf.path.root);
	len += strlen("%s/libexec/liblive-%s.so");

	path = malloc(len + 1);
	if (!path) {
		ErrPrint("heap-path: %s\n", strerror(errno));
		return -ENOMEM;
	}

	snprintf(path, len, "%s%s/libexec/liblive-%s.so", g_conf.path.root, pkgname, pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		free(path);
		return -EINVAL;
	}

	free(path);
	return 0;
}

int util_unlink(const char *filename)
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
		free(descfile);
		return -EFAULT;
	}

	(void)unlink(descfile);
	free(descfile);
	(void)unlink(filename);

	return 0;
}

/* End of a file */
