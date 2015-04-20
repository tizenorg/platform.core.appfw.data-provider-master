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

	*width = 0;
	*height = 0;
//	ecore_wl_screen_size_get();
	return WIDGET_ERROR_NOT_SUPPORTED;
}

int util_screen_init(void)
{
	s_info.initialized = 1;
//	ecore_wl_init();
	return WIDGET_ERROR_NONE;
}

int util_screen_fini(void)
{
	s_info.initialized = 0;
//	ecore_wl_shutdown();
	return WIDGET_ERROR_NONE;
}

/* End of a file */
