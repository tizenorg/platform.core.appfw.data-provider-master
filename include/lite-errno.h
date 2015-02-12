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
enum widget_status {
	WIDGET_STATUS_ERROR_NONE = 0x00000000, /**< Operation is successfully completed */
	WIDGET_STATUS_ERROR = 0x80000000, /**< This will be OR'd with other specific error value */
	WIDGET_STATUS_ERROR_INVALID_PARAMETER = WIDGET_STATUS_ERROR | 0x0001, /**< Invalid request */
	WIDGET_STATUS_ERROR_FAULT = WIDGET_STATUS_ERROR | 0x0002, /**< Fault - Unable to recover from the error */
	WIDGET_STATUS_ERROR_OUT_OF_MEMORY = WIDGET_STATUS_ERROR | 0x0004, /**< Memory is not enough to do this operation */
	WIDGET_STATUS_ERROR_EXIST = WIDGET_STATUS_ERROR | 0x0008, /**< Already exists */
	WIDGET_STATUS_ERROR_BUSY = WIDGET_STATUS_ERROR | 0x0010, /**< Busy so the operation is not started(accepted), try again */
	WIDGET_STATUS_ERROR_PERMISSION_DENIED = WIDGET_STATUS_ERROR | 0x0020, /**< Permission error */
	WIDGET_STATUS_ERROR_ALREADY = WIDGET_STATUS_ERROR | 0x0040, /**< Operation is already started */
	WIDGET_STATUS_ERROR_CANCEL = WIDGET_STATUS_ERROR | 0x0080, /**< Operation is canceled */
	WIDGET_STATUS_ERROR_IO_ERROR = WIDGET_STATUS_ERROR | 0x0100, /**< I/O Error */
	WIDGET_STATUS_ERROR_NOT_EXIST = WIDGET_STATUS_ERROR | 0x0200, /**< Not exists */
	WIDGET_STATUS_ERROR_TIMEOUT = WIDGET_STATUS_ERROR | 0x0400, /**< Timeout */
	WIDGET_STATUS_ERROR_NOT_IMPLEMENTED = WIDGET_STATUS_ERROR | 0x0800, /**< Operation is not implemented */
	WIDGET_STATUS_ERROR_NO_SPACE = WIDGET_STATUS_ERROR | 0x1000, /**< No space to operate */
	WIDGET_STATUS_ERROR_DISABLED = WIDGET_STATUS_ERROR | 0x2000 /**< Disabled */
};


/*!
 * \brief Check whether given code value indicates error or not.
 * \param[in] s
 * \return 1 or 0
 */
#define WIDGET_STATUS_IS_ERROR(s)	(!!((s) & WIDGET_STATUS_ERROR))

/*!
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif
/* End of a file */

