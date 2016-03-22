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

#define DELAY_TIME 0.0000001f
#define HAPI __attribute__((visibility("hidden")))


#if !defined(VCONFKEY_MASTER_RESTART_COUNT)
#define VCONFKEY_MASTER_RESTART_COUNT	"memory/private/data-provider-master/restart_count"
#endif

#define CONF_LOG_PATH "/tmp/.widget.service"
#define CONF_MAX_LOG_LINE 1000
#define CONF_MAX_LOG_FILE 3

#define CR 13
#define LF 10

/* End of a file */
