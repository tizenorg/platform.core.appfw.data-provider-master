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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/signalfd.h>
#include <ctype.h>

#include <systemd/sd-daemon.h>

#include <glib.h>
#include <glib-object.h>
#include <aul.h>
#include <vconf.h>
#include <Ecore.h>
#include <dlog.h>
#include <locale.h>

#include "debug.h"
#include "util.h"
#include "critical_log.h"
#include "service_common.h"
#include "notification_service.h"
#include "badge_service.h"
#ifndef WEARABLE
#include "shortcut_service.h"
#endif

static void lang_key_changed_cb(keynode_t *node EINA_UNUSED, void *first)
{
	char *lang;
	char *r;

	lang = vconf_get_str(VCONFKEY_LANGSET);
	if (lang) {
		setenv("LANG", lang, 1);
		setenv("LC_MESSAGES", lang, 1);
		r = setlocale(LC_ALL, "");
		if (r == NULL) {
			r = setlocale(LC_ALL, lang);
			if (r != NULL)
				DbgPrint("setlocale = %s", r);
		}
		DbgPrint("setlocale = %s", r);
		free(lang);
	}
}

static inline int app_create(void)
{
	int ret;

	ret = vconf_notify_key_changed(VCONFKEY_LANGSET, lang_key_changed_cb, NULL);
	if (ret < 0)
		DbgPrint("VCONFKEY_LANGSET notify key chenaged: %d\n", ret);

	lang_key_changed_cb(NULL, NULL);
#ifndef WEARABLE
	ret = shortcut_service_init();
	if (ret < 0)
		DbgPrint("shortcut: %d\n", ret);
#endif
	ret = notification_service_init();
	if (ret < 0)
		DbgPrint("noti: %d\n", ret);

	ret = badge_service_init();
	if (ret < 0)
		DbgPrint("badge: %d\n", ret);

	return 0;
}

static inline int app_terminate(void)
{
	int ret;

	ret = badge_service_fini();
	if (ret < 0)
		DbgPrint("badge: %d\n", ret);

	ret = notification_service_fini();
	if (ret < 0)
		DbgPrint("noti: %d\n", ret);
#ifndef WEARABLE
	ret = shortcut_service_fini();
	if (ret < 0)
		DbgPrint("shortcut: %d\n", ret);
#endif
	DbgPrint("Terminated\n");
	return 0;
}

static Eina_Bool signal_cb(void *data, Ecore_Fd_Handler *handler)
{
	struct signalfd_siginfo fdsi;
	ssize_t size;
	int cfd;
	int fd;

	fd = ecore_main_fd_handler_fd_get(handler);
	if (fd < 0) {
		ErrPrint("Unable to get FD\n");
		ecore_main_fd_handler_del(handler);
		return ECORE_CALLBACK_CANCEL;
	}

	size = read(fd, &fdsi, sizeof(fdsi));
	if (size != sizeof(fdsi)) {
		ErrPrint("read: %d\n", errno);
		ecore_main_fd_handler_del(handler);
		return ECORE_CALLBACK_CANCEL;
	}

	if (fdsi.ssi_signo == SIGTERM) {
		CRITICAL_LOG("Terminated(SIGTERM)\n");
		cfd = creat("/tmp/.stop.provider", 0644);
		if (cfd < 0 || close(cfd) < 0)
			ErrPrint("stop.provider: %d\n", errno);

		ecore_main_loop_quit();
	} else {
		CRITICAL_LOG("Unknown SIG[%d] received\n", fdsi.ssi_signo);
	}

	return ECORE_CALLBACK_RENEW;
}

int main(int argc, char *argv[])
{
	int ret;
	int restart_count = 0;
	sigset_t mask;
	Ecore_Fd_Handler *signal_handler = NULL;

	/* appcore_agent_terminate */
	if (ecore_init() <= 0)
		return -EFAULT;

	ecore_app_args_set(argc, (const char **)argv);

#if (GLIB_MAJOR_VERSION <= 2 && GLIB_MINOR_VERSION < 36)
	g_type_init();
#endif

	vconf_get_int(VCONFKEY_MASTER_RESTART_COUNT, &restart_count);
	util_setup_log_disk();

	/*!
	 * How could we care this return values?
	 * Is there any way to print something on the screen?
	 */
	ret = critical_log_init(util_basename(argv[0]));
	if (ret < 0)
		ErrPrint("Failed to init the critical log\n");

	sigemptyset(&mask);
	ret = sigaddset(&mask, SIGTERM);
	if (ret < 0)
		CRITICAL_LOG("sigaddset: %d\n", errno);

	ret = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (ret < 0)
		CRITICAL_LOG("sigprocmask: %d\n", errno);

	ret = signalfd(-1, &mask, 0);
	if (ret < 0) {
		CRITICAL_LOG("signalfd: %d\n", errno);
	} else {
		signal_handler = ecore_main_fd_handler_add(
				ret, ECORE_FD_READ, signal_cb,
				NULL, NULL, NULL);
		CRITICAL_LOG("Signal handler initiated: %d\n", ret);
	}

	app_create();
	sd_notify(0, "READY=1");

	restart_count++;
	vconf_set_int(VCONFKEY_MASTER_RESTART_COUNT, restart_count);

	ecore_main_loop_begin();

	app_terminate();

	if (signal_handler)
		ecore_main_fd_handler_del(signal_handler);

	ecore_shutdown();
	critical_log_fini();

	return 0;
}

/* End of a file */
