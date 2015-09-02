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

extern int dead_init(void);
extern int dead_fini(void);
extern int dead_callback_add(int handle, void (*dead_cb)(int handle, void *data), void *data);
extern void *dead_callback_del(int handle, void (*dead_cb)(int handle, void *data), void *data);

/* End of a file */
