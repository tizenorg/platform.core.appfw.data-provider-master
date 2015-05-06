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

struct widget_mgr;

extern int widget_mgr_init(void);
extern void widget_mgr_fini(void);
extern struct widget_mgr *widget_mgr_create(pid_t pid, int handle);
extern void widget_mgr_destroy(struct widget_mgr *info);

extern struct widget_mgr *widget_mgr_find_by_pid(pid_t pid);
extern struct widget_mgr *widget_mgr_find_by_handle(int handle);

extern const char *widget_mgr_filename(struct widget_mgr *info);
extern pid_t widget_mgr_pid(struct widget_mgr *info);
extern FILE *widget_mgr_fifo(struct widget_mgr *info);
extern int widget_mgr_open_fifo(struct widget_mgr *info);
extern void widget_mgr_close_fifo(struct widget_mgr *info);
extern void widget_mgr_set_data(struct widget_mgr *info, void *data);
extern void *widget_mgr_data(struct widget_mgr *info);

extern int widget_mgr_is_valid_requestor(pid_t pid);
/* End of a file */
