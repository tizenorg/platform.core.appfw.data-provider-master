#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
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
#include "abi.h"

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

	/*!
	 * \note
	 * Should be replaced this with for loading from INI (or xml or DB).
	 */
	if (abi_add_entry("c", SLAVE_PKGNAME) != 0)
		ErrPrint("Failed to add a slave package for \"c\"\n");

	if (abi_add_entry("cpp", SLAVE_PKGNAME) != 0)
		ErrPrint("Failed to add a slave package for \"cpp\"\n");

	if (abi_add_entry("html", SLAVE_WEB_PKGNAME) != 0)
		ErrPrint("Failed to add a slave package for \"html\"\n");

	return 0;
}

int io_fini(void)
{
	abi_del_entry("c");
	abi_del_entry("cpp");
	abi_del_entry("html");
	return 0;
}

/* End of a file */
