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
#include "slave_manager.h"
#include "pkg_manager.h"
#include "group.h"
#include "dead_monitor.h"
#include "conf.h"
#include "ctx_client.h"
#include "io.h"
#include "xmonitor.h"
#include "script_handler.h"

#if defined(FLOG)
FILE *__file_log_fp;
#endif

static int slave_deactivate_cb(struct slave_node *slave, void *data)
{
	slave_activate(slave);
	return EXIT_SUCCESS;
}

static inline int app_create(void *data)
{
	int ret;

	conf_update_size();

	ret = dbus_init();
	if (ret < 0)
		DbgPrint("Failed to initialize the dbus\n");

	ret = slave_manager_init();
	if (ret < 0)
		DbgPrint("Failed to initialize the slave manager\n");

	ret = pkgmgr_init();
	if (ret < 0)
		DbgPrint("Failed to initialize the pkgmgr\n");

	ret = slave_add_deactivate_cb(slave_deactivate_cb, NULL);
	if (ret < 0)
		DbgPrint("Failed to add deactivate callback\n");

	ret = dead_init();
	DbgPrint("Dead callback is registered: %d\n", ret);

	ret = group_init();
	DbgPrint("group init: %d\n", ret);

	ret = io_init();
	DbgPrint("Init I/O: %d\n", ret);

	ret = ctx_client_init();
	DbgPrint("Context engine is initialized: %d\n", ret);

	xmonitor_init();
	DbgPrint("XMonitor init is done\n");

	return 0;
}

static inline int app_terminate(void *data)
{
	int ret;
	void *cbdata;

	xmonitor_fini();

	ret = ctx_client_fini();
	DbgPrint("ctx_client_fini returns %d\n", ret);

	cbdata = slave_del_deactivate_cb(slave_deactivate_cb);
	DbgPrint("cbdata of slave deactivate callback is %p\n", cbdata);

	ret = pkgmgr_fini();
	if (ret < 0)
		DbgPrint("Failed to finalize the pkgmgr\n");
	else
		DbgPrint("pkgmgr finalized\n");

	ret = slave_manager_fini();
	if (ret < 0)
		DbgPrint("Failed to finalize the slave manager\n");
	else
		DbgPrint("Slave manager finalized\n");

	ret = dbus_fini();
	if (ret < 0)
		DbgPrint("Failed to finalize the dbus\n");
	else
		DbgPrint("DBUS finialized\n");

	dead_fini();
	DbgPrint("dead signal handler finalized\n");

	io_fini();
	DbgPrint("IO finalized\n");

	group_fini();
	DbgPrint("Group finalized\n");

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
	int ret;

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
	return ret;
}

/* End of a file */
