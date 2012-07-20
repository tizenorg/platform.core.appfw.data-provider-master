#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <unistd.h>

#include "critical_log.h"

static struct {
	FILE *fp;
} s_info = {
	.fp = NULL,
};



int critical_log(const char *func, int line, const char *fmt, ...)
{
	va_list ap;
	int ret;
	struct timeval tv;

	if (!s_info.fp)
		return -EIO;

	gettimeofday(&tv, NULL);
	fprintf(s_info.fp, "%d %lu.%lu [%s:%d] ", getpid(), tv.tv_sec, tv.tv_usec, basename((char *)func), line);

	va_start(ap, fmt);
	ret = vfprintf(s_info.fp, fmt, ap);
	va_end(ap);
	return ret;
}



int critical_log_init(void)
{
	if (s_info.fp)
		return 0;

	s_info.fp = fopen("/opt/var/log/provider-master.log", "a+");
	if (!s_info.fp) {
		fprintf(stderr, "Failed to open log: %s\n", strerror(errno));
		return -EIO;
	}

	return 0;
}



int critical_log_fini(void)
{
	if (s_info.fp)
		fclose(s_info.fp);

	s_info.fp = NULL;
	return 0;
}



/* End of a file */
