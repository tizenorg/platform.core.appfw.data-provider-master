/*
 * com.samsung.live-data-provider
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: Sung-jae Park <nicesj.park@samsung.com>, Youngjoo Park <yjoo93.park@samsung.com>
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
 *
 */

struct item;

extern struct item *parser_load(const char *filename);
extern int parser_unload(struct item *handle);
extern double parser_period(struct item *handle);
extern int parser_network(struct item *handle);
extern int parser_timeout(struct item *handle);
extern int parser_auto_launch(struct item *handle);
extern unsigned int parser_size(struct item *handle);
extern void parser_get_pdsize(struct item *handle, unsigned int *width, unsigned int *height);
extern const char *parser_group_str(struct item *handle);
extern int parser_secured(struct item *handle);
extern int parser_pinup(struct item *handler);

extern const char *parser_lb_path(struct item *handle);
extern const char *parser_lb_group(struct item *handle);
extern const char *parser_pd_path(struct item *handle);
extern const char *parser_pd_group(struct item *handle);

extern int parser_text_pd(struct item *handle);
extern int parser_text_lb(struct item *handle);

extern char *parser_script(struct item *handle);

/* End of a file */
