#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <dlog.h>

#include "debug.h"
#include "conf.h"
#include "pkg_manager.h"
#include "io.h"
#include "parser.h"
#include "group.h"
#include "util.h"

int errno;

int io_init(void)
{
	struct dirent *ent;
	DIR *dir;
	struct item *item;

	dir = opendir(g_conf.path.root);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;

		if (util_validate_livebox_package(ent->d_name) < 0)
			continue;

		item = parser_load(ent->d_name);
		if (item) {
			const char *groups;

			groups = parser_group_str(item);
			if (groups && group_add_livebox(groups, ent->d_name) < 0)
				ErrPrint("Something goes wrong with group string[%s]\n", groups);

			parser_unload(item);
		}
	}

	closedir(dir);
	return 0;
}

int io_fini(void)
{
	return 0;
}

/* End of a file */
