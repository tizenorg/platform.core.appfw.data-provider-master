#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <dlog.h>
#include <Eina.h>

#include "debug.h"
#include "conf.h"
#include "io.h"
#include "parser.h"
#include "group.h"
#include "util.h"
#include "client_life.h"
#include "slave_life.h"
#include "package.h"

int errno;

int io_init(void)
{
	struct dirent *ent;
	DIR *dir;
	struct pkg_info *info;

	dir = opendir(g_conf.path.root);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;

		info = package_create(ent->d_name);
		if (info)
			DbgPrint("[%s] information is built\n", ent->d_name);
	}

	closedir(dir);
	return 0;
}

int io_fini(void)
{
	return 0;
}

/* End of a file */
