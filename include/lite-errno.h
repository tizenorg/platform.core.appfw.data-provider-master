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

#ifndef __WIDGET_ERRNO_H
#define __WIDGET_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup CAPI_WIDGET_SERVICE_MODULE
 * @{
 */

/**
 * @brief
 * Definitions for the result status of widget operation.
 */
#if 0
enum widget_status {
	WIDGET_ERROR_NONE = 0x00000000, /**< Operation is successfully completed */
	WIDGET_STATUS_ERROR = 0x80000000, /**< This will be OR'd with other specific error value */
	WIDGET_ERROR_INVALID_PARAMETER = WIDGET_STATUS_ERROR | 0x0001, /**< Invalid request */
	WIDGET_ERROR_FAULT = WIDGET_STATUS_ERROR | 0x0002, /**< Fault - Unable to recover from the error */
	WIDGET_ERROR_OUT_OF_MEMORY = WIDGET_STATUS_ERROR | 0x0004, /**< Memory is not enough to do this operation */
	WIDGET_ERROR_ALREADY_EXIST = WIDGET_STATUS_ERROR | 0x0008, /**< Already exists */
	WIDGET_ERROR_RESOURCE_BUSY = WIDGET_STATUS_ERROR | 0x0010, /**< Busy so the operation is not started(accepted), try again */
	WIDGET_ERROR_PERMISSION_DENIED = WIDGET_STATUS_ERROR | 0x0020, /**< Permission error */
	WIDGET_ERROR_ALREADY = WIDGET_STATUS_ERROR | 0x0040, /**< Operation is already started */
	WIDGET_ERROR_CANCELED = WIDGET_STATUS_ERROR | 0x0080, /**< Operation is canceled */
	WIDGET_ERROR_IO_ERROR = WIDGET_STATUS_ERROR | 0x0100, /**< I/O Error */
	WIDGET_ERROR_NOT_EXIST = WIDGET_STATUS_ERROR | 0x0200, /**< Not exists */
	WIDGET_ERROR_TIMEOUT = WIDGET_STATUS_ERROR | 0x0400, /**< Timeout */
	WIDGET_ERROR_NOT_SUPPORTED = WIDGET_STATUS_ERROR | 0x0800, /**< Operation is not implemented */
	WIDGET_ERROR_NO_SPACE = WIDGET_STATUS_ERROR | 0x1000, /**< No space to operate */
	WIDGET_ERROR_DISABLED = WIDGET_STATUS_ERROR | 0x2000 /**< Disabled */
};
#endif

/*!
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif
/* End of a file */

