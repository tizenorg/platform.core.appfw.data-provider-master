#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <Ecore.h>
#include <Ecore_X.h>
#include <Evas.h>
#include <Ecore_Evas.h>
#include <glib.h>
#include <glib-object.h>
#include <aul.h>

#include <packet.h>
#include <dlog.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include <buffer_handler.h>
#include "script_handler.h"
#include "package.h"
#include "group.h"
#include "dead_monitor.h"
#include "conf.h"
#include "ctx_client.h"
#include "io.h"
#include "xmonitor.h"
#include "setting.h"
#include "server.h"
#include "util.h"
#include "debug.h"
#include "critical_log.h"

#if defined(FLOG)
FILE *__file_log_fp;
#endif

static inline int app_create(void *data)
{
	int ret;

	conf_update_size();

	if (access(SLAVE_LOG_PATH, R_OK|W_OK) != 0) {
		mkdir(SLAVE_LOG_PATH, 755);
	}

	/*!
	 * \note
	 * Dead signal handler has to be initialized before
	 * initate package or client (slave and client).
	 *
	 * Because while creating slaves for packages.
	 * It could be crashed before complete the initation stage.
	 *
	 * Then the dead callback should be invoked to handle it properly.
	 *
	 * To enable the dead signal handler,
	 * dead_init should be done before other components are initiated.
	 */
	ret = setting_init();
	DbgPrint("Setting initialized: %d\n", ret);

	ret = client_init();
	DbgPrint("Client initialized: %d\n", ret);

	ret = dead_init();
	DbgPrint("Dead callback is registered: %d\n", ret);

	ret = group_init();
	DbgPrint("group init: %d\n", ret);

	ret = io_init();
	DbgPrint("Init I/O: %d\n", ret);

	ret = package_init();
	DbgPrint("pkgmgr initialized: %d\n", ret);

	ret = ctx_client_init();
	DbgPrint("Context engine is initialized: %d\n", ret);

	ret = xmonitor_init();
	DbgPrint("XMonitor init is done: %d\n", ret);

	ret = buffer_handler_init();
	DbgPrint("Buffer handler init is done: %d\n", ret);

	/*!
	 * \note
	 * After initiate all other sub-systtems,
	 * Enable the server socket.
	 */
	ret = server_init();
	DbgPrint("Server initialized: %d\n", ret);

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

	ret = client_fini();
	DbgPrint("Finalize client connections : %d\n", ret);

	ret = server_fini();
	DbgPrint("Finalize dbus: %d\n", ret);

	ret = dead_fini();
	DbgPrint("dead signal handler finalized: %d\n", ret);

	ret = io_fini();
	DbgPrint("IO finalized: %d\n", ret);

	ret = group_fini();
	DbgPrint("Group finalized: %d\n", ret);

	DbgPrint("Terminated\n");
	return 0;
}

/*
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
*/

static void signal_handler(int signum, siginfo_t *info, void *unused)
{
	CRITICAL_LOG("Terminated(SIGTERM)\n");
	creat("/tmp/.stop.provider", 0644);
	exit(0);
}

int main(int argc, char *argv[])
{
	struct sigaction act;
	int ret;

	/*!
	 * How could we care this return values?
	 * Is there any way to print something on the screen?
	 */
	ret = critical_log_init(util_basename(argv[0]));
	if (ret < 0)
		fprintf(stderr, "Failed to init the critical log\n");

	/*
	ret = daemon(0, 0);
	if (ret < 0)
		CRITICAL_LOG("Failed to make daemon: %s\n", strerror(errno));
	*/

#if defined(FLOG)
	__file_log_fp = fopen("/tmp/live.log", "w+t");
	if (!__file_log_fp)
		__file_log_fp = fdopen(1, "w+t");
#endif
	/* appcore_agent_terminate */
	if (ecore_init() <= 0) {
		CRITICAL_LOG("Failed to initiate ecore\n");
		critical_log_fini();
		return -EFAULT;
	}

	act.sa_sigaction = signal_handler;
	act.sa_flags = SA_SIGINFO;

	ret = sigemptyset(&act.sa_mask);
	if (ret < 0)
		CRITICAL_LOG("Failed to do sigemptyset: %s\n", strerror(errno));

	ret = sigaddset(&act.sa_mask, SIGTERM);
	if (ret < 0)
		CRITICAL_LOG("Failed to mask the SIGTERM: %s\n", strerror(errno));

	ret = sigaction(SIGTERM, &act, NULL);
	if (ret < 0)
		CRITICAL_LOG("Failed to add sigaction: %s\n", strerror(errno));

	if (ecore_x_init(NULL) <= 0) {
		CRITICAL_LOG("Failed to ecore x init\n");
		ecore_shutdown();
		critical_log_fini();
		return -EFAULT;
	}

	ecore_app_args_set(argc, (const char **)argv);

	if (evas_init() <= 0) {
		CRITICAL_LOG("Failed to init evas return count is below than 0\n");
		ecore_x_shutdown();
		ecore_shutdown();
		critical_log_fini();
		return -EFAULT;
	}

	if (ecore_evas_init() <= 0) {
		CRITICAL_LOG("Failed to init ecore_evas\n");
		evas_shutdown();
		ecore_x_shutdown();
		ecore_shutdown();
		critical_log_fini();
		return -EFAULT;
	}

	g_type_init();

	script_init();

//	aul_launch_init(aul_handler_cb, NULL);
//	aul_launch_argv_handler(argc, argv);

	app_create(NULL);
	ecore_main_loop_begin();
	app_terminate(NULL);

	script_fini();

	ecore_evas_shutdown();
	evas_shutdown();

	ecore_x_shutdown();
	ecore_shutdown();
	critical_log_fini();
	return 0;
}

/* End of a file */
