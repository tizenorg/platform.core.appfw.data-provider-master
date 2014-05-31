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

#ifndef __LIVEBOX_ERRNO_H
#define __LIVEBOX_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * \addtogroup CAPI_LIVEBOX_SERVICE_MODULE
 * \{
 */

/*!
 * \brief
 * Definitions for the result status of livebox operation.
 */
enum livebox_status {
	LB_STATUS_SUCCESS = 0x00000000, /*!< Operation is successfully completed */
	LB_STATUS_ERROR = 0x80000000, /*!< This will be OR'd with other specific error value */
	LB_STATUS_ERROR_INVALID = LB_STATUS_ERROR | 0x0001, /*!< Invalid request */
	LB_STATUS_ERROR_FAULT = LB_STATUS_ERROR | 0x0002, /*!< Fault - Unable to recover from the error */
	LB_STATUS_ERROR_MEMORY = LB_STATUS_ERROR | 0x0004, /*!< Memory is not enough to do this operation */
	LB_STATUS_ERROR_EXIST = LB_STATUS_ERROR | 0x0008, /*!< Already exists */
	LB_STATUS_ERROR_BUSY = LB_STATUS_ERROR | 0x0010, /*!< Busy so the operation is not started(accepted), try again */
	LB_STATUS_ERROR_PERMISSION = LB_STATUS_ERROR | 0x0020, /*!< Permission error */
	LB_STATUS_ERROR_ALREADY = LB_STATUS_ERROR | 0x0040, /*!< Operation is already started */
	LB_STATUS_ERROR_CANCEL = LB_STATUS_ERROR | 0x0080, /*!< Operation is canceled */
	LB_STATUS_ERROR_IO = LB_STATUS_ERROR | 0x0100, /*!< I/O Error */
	LB_STATUS_ERROR_NOT_EXIST = LB_STATUS_ERROR | 0x0200, /*!< Not exists */
	LB_STATUS_ERROR_TIMEOUT = LB_STATUS_ERROR | 0x0400, /*!< Timeout */
	LB_STATUS_ERROR_NOT_IMPLEMENTED = LB_STATUS_ERROR | 0x0800, /*!< Operation is not implemented */
	LB_STATUS_ERROR_NO_SPACE = LB_STATUS_ERROR | 0x1000, /*!< No space to operate */
	LB_STATUS_ERROR_DISABLED = LB_STATUS_ERROR | 0x2000 /*!< Disabled */
};

/*!
 * \brief Check whether given code value indicates error or not.
 * \param[in] s
 * \return 1 or 0
 */
#define LB_STATUS_IS_ERROR(s)	(!!((s) & LB_STATUS_ERROR))

/*!
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif
/* End of a file */

