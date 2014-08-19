/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>

#include <vconf.h>
#include <dlog.h>

#include <Ecore.h>
#include <Eina.h>

#include "setting.h"
#include "util.h"
#include "debug.h"
#include "conf.h"
#include "critical_log.h"

#if defined(HAVE_LIVEBOX)
#include <livebox-service.h>
#include "client_life.h"
#include "slave_life.h"
#include "xmonitor.h"
#include "package.h"
#include "instance.h"
#else
#define xmonitor_handle_state_changes()
#endif

int errno;

static struct {
	int deactivated;
} s_info = {
	.deactivated = 0,
};

static void lcd_state_cb(keynode_t *node, void *user_data)
{
	if (!node) {
		return;
	}

	xmonitor_handle_state_changes();
}

HAPI int setting_is_lcd_off(void)
{
	int state;

	if (vconf_get_int(VCONFKEY_PM_STATE, &state) != 0) {
		ErrPrint("Idle lock state is not valid\n");
		state = VCONFKEY_PM_STATE_NORMAL; /* UNLOCK */
	}

	ErrPrint("State: %d, (%d:lcdoff, %d:sleep)\n", state, VCONFKEY_PM_STATE_LCDOFF, VCONFKEY_PM_STATE_SLEEP);
	return state == VCONFKEY_PM_STATE_LCDOFF || state == VCONFKEY_PM_STATE_SLEEP;
}

static void power_off_cb(keynode_t *node, void *user_data)
{
 	int val;
	CRITICAL_LOG("Terminated(vconf)\n");

	if (vconf_get_int(VCONFKEY_SYSMAN_POWER_OFF_STATUS, &val) != 0) {
		ErrPrint("Failed to get power off status (%d)\n", val);
		return;
	}

	if (val == VCONFKEY_SYSMAN_POWER_OFF_DIRECT || val == VCONFKEY_SYSMAN_POWER_OFF_RESTART) {
		DbgPrint("Power off requested: Ignored\n");
	} else {
		ErrPrint("Unknown power state: %d\n", val);
	}
}

static void region_changed_cb(keynode_t *node, void *user_data)
{
        char *region;
        char *r;

        region = vconf_get_str(VCONFKEY_REGIONFORMAT);
        if (!region) {
		return;
	}

	setenv("LC_CTYPE", region, 1);
	setenv("LC_NUMERIC", region, 1);
	setenv("LC_TIME", region, 1);
	setenv("LC_COLLATE", region, 1);
	setenv("LC_MONETARY", region, 1);
	setenv("LC_PAPER", region, 1);
	setenv("LC_NAME", region, 1);
	setenv("LC_ADDRESS", region, 1);
	setenv("LC_TELEPHONE", region, 1);
	setenv("LC_MEASUREMENT", region, 1);
	setenv("LC_IDENTIFICATION", region, 1);

	r = setlocale(LC_ALL, "");
	if (r == NULL) {
		ErrPrint("Failed to change region\n");
	}

	DbgFree(region);
}

static void lang_changed_cb(keynode_t *node, void *user_data)
{
        char *lang;
       	char *r;

        lang = vconf_get_str(VCONFKEY_LANGSET);
        if (!lang) {
		return;
	}

	setenv("LANG", lang, 1);
	setenv("LC_MESSAGES", lang, 1);

	r = setlocale(LC_ALL, "");
	if (!r) {
		ErrPrint("Failed to change locale\n");
	}

	DbgPrint("Locale: %s\n", setlocale(LC_ALL, NULL));
	DbgFree(lang);
}

static void low_mem_cb(keynode_t *node, void *user_data)
{
	int val;

	val = vconf_keynode_get_int(node);

	if (val >= VCONFKEY_SYSMAN_LOW_MEMORY_SOFT_WARNING)     {
		CRITICAL_LOG("Low memory: level %d\n", val);
		if (s_info.deactivated == 0) {
			s_info.deactivated = 1;
			//slave_deactivate_all(0, 1, 0);
			malloc_trim(0);
			ErrPrint("Fall into the low mem status\n");
		}
	} else {
		CRITICAL_LOG("Normal memory: level %d\n", val);
		if (s_info.deactivated == 1) {
			s_info.deactivated = 0;
			//slave_activate_all();
			ErrPrint("Recover from the low mem status\n");
		}
	}
}

HAPI int setting_init(void)
{
	int ret;

	ret = vconf_notify_key_changed(VCONFKEY_PM_STATE, lcd_state_cb, NULL);
	if (ret < 0) {
		ErrPrint("Failed to add vconf for lock state: %d\n", ret);
	}

	ret = vconf_notify_key_changed(VCONFKEY_SYSMAN_POWER_OFF_STATUS, power_off_cb, NULL);
	if (ret < 0) {
		ErrPrint("Failed to add vconf for power state: %d \n", ret);
	}

	ret = vconf_notify_key_changed(VCONFKEY_LANGSET, lang_changed_cb, NULL);
	if (ret < 0) {
		ErrPrint("Failed to add vconf for lang change: %d\n", ret);
	}

	ret = vconf_notify_key_changed(VCONFKEY_REGIONFORMAT, region_changed_cb, NULL);
	if (ret < 0) {
		ErrPrint("Failed to add vconf for region change: %d\n", ret);
	}

	ret = vconf_notify_key_changed(VCONFKEY_SYSMAN_LOW_MEMORY, low_mem_cb, NULL);
	if (ret < 0) {
		ErrPrint("Failed to add vconf for low mem monitor: %d\n", ret);
	}

	lang_changed_cb(NULL, NULL);
	region_changed_cb(NULL, NULL);
	return ret;
}

HAPI int setting_fini(void)
{
	int ret;

	ret = vconf_ignore_key_changed(VCONFKEY_SYSMAN_LOW_MEMORY, low_mem_cb);
	if (ret < 0) {
		ErrPrint("Failed to ignore vconf key (%d)\n", ret);
	}

	ret = vconf_ignore_key_changed(VCONFKEY_REGIONFORMAT, region_changed_cb);
	if (ret < 0) {
		ErrPrint("Failed to ignore vconf key (%d)\n", ret);
	}

	ret = vconf_ignore_key_changed(VCONFKEY_LANGSET, lang_changed_cb);
	if (ret < 0) {
		ErrPrint("Failed to ignore vconf key (%d)\n", ret);
	}

	ret = vconf_ignore_key_changed(VCONFKEY_PM_STATE, lcd_state_cb);
	if (ret < 0) {
		ErrPrint("Failed to ignore vconf key (%d)\n", ret);
	}

	ret = vconf_ignore_key_changed(VCONFKEY_SYSMAN_POWER_OFF_STATUS, power_off_cb);
	if (ret < 0) {
		ErrPrint("Failed to ignore vconf key (%d)\n", ret);
	}

	return ret;
}

/* End of a file */
