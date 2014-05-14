#include <Ecore_X.h>

#if defined(HAVE_LIVEBOX)
#include <livebox-errno.h>
#else
#include "lite-errno.h"
#endif
#include "util.h"

int util_screen_size_get(int *width, int *height)
{
	ecore_x_window_size_get(0, width, height);
	return LB_STATUS_SUCCESS;
}

int util_screen_init(void)
{
	return ecore_x_init(NULL);
}

int util_screen_fini(void)
{
	return ecore_x_shutdown();
}

/* End of a file */

