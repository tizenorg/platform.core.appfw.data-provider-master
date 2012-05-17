/*
 * com.samsung.data-provider-master
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

extern int virtual_canvas_flush_to_file(Evas *e, const char *filename, int w, int h);
extern int virtual_canvas_flush_data_to_file(Evas *e, char *data, const char *filename, int w, int h);

extern Evas *virtual_canvas_create(int w, int h);
extern int virtual_canvas_destroy(Evas *e);

/* End of a file */
