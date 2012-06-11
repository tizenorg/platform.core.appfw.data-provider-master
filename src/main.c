#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>

#include <Ecore.h>
#include <Ecore_X.h>
#include <Evas.h>
#include <Ecore_Evas.h>
#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <aul.h>

#include <dlog.h>

#include "debug.h"
#include "dbus.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "script_handler.h"
#include "package.h"
#include "group.h"
#include "dead_monitor.h"
#include "conf.h"
#include "ctx_client.h"
#include "io.h"
#include "xmonitor.h"
#include "setting.h"

#if defined(FLOG)
FILE *__file_log_fp;
#endif

static inline int app_create(void *data)
{
	static int initialized = 0;
	int ret;

	if (initialized == 1) {
		DbgPrint("Already initialized\n");
		return 0;
	}

	ret = ctx_client_init();
	DbgPrint("Context engine is initialized: %d\n", ret);

	conf_update_size();

	ret = dbus_init();
	DbgPrint("dbus initialized: %d\n", ret);

	ret = package_init();
	DbgPrint("pkgmgr initialized: %d\n", ret);

	ret = dead_init();
	DbgPrint("Dead callback is registered: %d\n", ret);

	ret = group_init();
	DbgPrint("group init: %d\n", ret);

	ret = io_init();
	DbgPrint("Init I/O: %d\n", ret);

	ret = xmonitor_init();
	DbgPrint("XMonitor init is done: %d\n", ret);

	ret = setting_init();
	DbgPrint("Setting initialized: %d\n", ret);

	ret = client_init();
	DbgPrint("Client initialized: %d\n", ret);

	if (access(g_conf.path.slave_log, R_OK|W_OK) != 0) {
		mkdir(g_conf.path.slave_log, 755);
	}

	initialized = 1;

	return 0;
}

static inline int app_terminate(void *data)
{
	int ret;

	ret = setting_fini();
	DbgPrint("Finalize setting : %d\n", ret);

	xmonitor_fini();

	ret = ctx_client_fini();
	DbgPrint("ctx_client_fini returns %d\n", ret);

	ret = package_fini();
	DbgPrint("Finalize package info: %d\n", ret);

	ret = dbus_fini();
	DbgPrint("Finalize dbus: %d\n", ret);

	ret = dead_fini();
	DbgPrint("dead signal handler finalized: %d\n", ret);

	ret = io_fini();
	DbgPrint("IO finalized: %d\n", ret);

	ret = group_fini();
	DbgPrint("Group finalized: %d\n", ret);

	ret = client_fini();
	DbgPrint("Finalize client connections : %d\n", ret);

	DbgPrint("Terminated\n");
	return 0;
}

static int aul_handler_cb(aul_type type, bundle *kb, void *data)
{
	int ret;
	switch (type) {
	case AUL_START:
		ret = app_create(data);
		break;
	case AUL_RESUME:
		ret = -ENOSYS;
		break;
	case AUL_TERMINATE:
		ret = app_terminate(data);
		ecore_main_loop_quit();
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int main(int argc, char *argv[])
{
#if defined(FLOG)
	__file_log_fp = fopen("/tmp/live.log", "w+t");
	if (!__file_log_fp)
		__file_log_fp = fdopen(1, "w+t");
#endif
	/* appcore_agent_terminate */
	if (ecore_init() < 0) {
		ErrPrint("Failed to initiate ecore\n");
		return -EFAULT;
	}
	if (ecore_x_init(NULL) < 0) {
		ErrPrint("Failed to ecore x init\n");
		ecore_shutdown();
		return -EFAULT;
	}

	ecore_app_args_set(argc, (const char **)argv);

        evas_init();
	ecore_evas_init();
	g_type_init();

	script_init();

	aul_launch_init(aul_handler_cb, NULL);
	aul_launch_argv_handler(argc, argv);

	ecore_main_loop_begin();

	script_fini();

	ecore_evas_shutdown();
	evas_shutdown();

	ecore_x_shutdown();
	ecore_shutdown();
	return 0;
}

/* End of a file */
