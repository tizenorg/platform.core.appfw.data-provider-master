#include "util.h"

#if defined(HAVE_LIVEBOX)
#include <livebox-errno.h>
#else
#include "lite-errno.h"
#endif

int util_screen_size_get(int *width, int *height)
{
	*width = 0;
	*height = 0;
	return LB_STATUS_ERROR_NOT_IMPLEMENTED;
}

int util_screen_init(void)
{
	return LB_STATUS_SUCCESS;
}

int util_screen_fini(void)
{
	return LB_STATUS_SUCCESS;
}

/* End of a file */
