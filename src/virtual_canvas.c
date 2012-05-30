/*
 * data-provider-master
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

#include <stdio.h>
#include <libgen.h>
#include <fcntl.h>
#include <unistd.h>

#include <Evas.h>
#include <Ecore_X.h>
#include <Ecore_Evas.h>

#include <dlog.h>

#include "virtual_canvas.h"
#include "debug.h"
#include "conf.h"

Evas *virtual_canvas_create(int w, int h)
{
	Ecore_Evas *internal_ee;
	Evas *internal_e;

	/* Create virtual canvas */
	internal_ee = ecore_evas_buffer_new(w, h);
	if (!internal_ee) {
		ErrPrint("Failed to create a new canvas buffer\n");
		return NULL;
	}

	ecore_evas_alpha_set(internal_ee, EINA_TRUE);
	ecore_evas_manual_render_set(internal_ee, EINA_TRUE);

	/* Get the "Evas" object from a virtual canvas */
	internal_e = ecore_evas_get(internal_ee);
	if (!internal_e) {
		ecore_evas_free(internal_ee);
		ErrPrint("Faield to get Evas object\n");
		return NULL;
	}

	return internal_e;
}

int virtual_canvas_flush_to_file(Evas *e, const char *filename, int w, int h)
{
	void *data;
	Ecore_Evas *internal_ee;
	int ret;

	internal_ee = ecore_evas_ecore_evas_get(e);
	if (!internal_ee) {
		ErrPrint("Failed to get ecore evas\n");
		return -EFAULT;
	}

	ecore_evas_manual_render(internal_ee);

	/* Get a pointer of a buffer of the virtual canvas */
	data = (void *)ecore_evas_buffer_pixels_get(internal_ee);
	if (!data) {
		ErrPrint("Failed to get pixel data\n");
		return -EFAULT;
	}

	ret = virtual_canvas_flush_data_to_file(e, data, filename, w, h);
	return ret;
}

int virtual_canvas_flush_data_to_file(Evas *e, char *data, const char *filename, int w, int h)
{
	Evas_Object *output;

	output = evas_object_image_add(e);
	if (!output) {
		ErrPrint("Failed to create an image object\n");
		return -EFAULT;
	}

	evas_object_image_data_set(output, NULL);
	evas_object_image_colorspace_set(output, EVAS_COLORSPACE_ARGB8888);
	evas_object_image_alpha_set(output, EINA_TRUE);
	evas_object_image_size_set(output, w, h);
	evas_object_image_smooth_scale_set(output, EINA_TRUE);
	evas_object_image_data_set(output, data);
	evas_object_image_data_update_add(output, 0, 0, w, h);

	if (evas_object_image_save(output, filename, NULL, g_conf.quality)
								== EINA_FALSE) {
		evas_object_del(output);
		ErrPrint("Faield to save a captured image (%s)\n", filename);
		return -EFAULT;
	}

	evas_object_del(output);

	if (access(filename, F_OK) != 0) {
		ErrPrint("File %s is not found (%s)\n", filename, strerror(errno));
		return -EFAULT;
	}

	return 0;
}

int virtual_canvas_destroy(Evas *e)
{
	Ecore_Evas *ee;

	ee = ecore_evas_ecore_evas_get(e);
	if (!ee) {
		ErrPrint("Failed to ecore evas object\n");
		return -EFAULT;
	}

	ecore_evas_free(ee);
	return 0;
}

/* End of a file */
