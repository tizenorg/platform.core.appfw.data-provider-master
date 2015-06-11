#include <Eina.h>
#include <Ecore_Wayland.h>

#include <dlog.h>

#include "debug.h"
#include "util.h"

#if defined(HAVE_LIVEBOX)
#include <widget_errno.h>
#else
#include "lite-errno.h"
#endif

static struct info {
	int initialized;
} s_info = {
	.initialized = 0,
};

int util_screen_size_get(int *width, int *height)
{
	if (!s_info.initialized) {
		ErrPrint("Not initialized\n");
		return WIDGET_ERROR_FAULT;
	}

	ecore_wl_screen_size_get(width, height);
	return WIDGET_ERROR_NONE;
}

int util_screen_init(void)
{
	s_info.initialized = 1;
	return ecore_wl_init(NULL);
}

int util_screen_fini(void)
{
	s_info.initialized = 0;
	return ecore_wl_shutdown();
}

/* End of a file */
