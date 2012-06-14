#include <stdio.h>
#include <unistd.h>

#include <vconf.h>
#include <dlog.h>

#include "setting.h"
#include "util.h"
#include "debug.h"
#include "slave_life.h"

static void lock_state_cb(keynode_t *node, void *user_data)
{
	if (!node)
		return;

	slave_check_pause_or_resume();
}

int setting_is_locked(void)
{
	int state;

	if (vconf_get_int(VCONFKEY_IDLE_LOCK_STATE, &state) != 0) {
		ErrPrint("Idle lock state is not valid\n");
		state = 0; /* UNLOCK */
	}

	return state;
}

int setting_init(void)
{
	int ret;
	ret = vconf_notify_key_changed(VCONFKEY_IDLE_LOCK_STATE, lock_state_cb, NULL);
	if (ret < 0)
		ErrPrint("Failed to add vconf for lock state\n");

	return ret;
}

int setting_fini(void)
{
	int ret;
	ret = vconf_ignore_key_changed(VCONFKEY_IDLE_LOCK_STATE, lock_state_cb);
	return ret;
}

/* End of a file */
