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

struct event_data {
	int x;
	int y;
	unsigned int keycode;
	int device;
	int slot;
	struct {
		int major;
		int minor;
	} touch;
	struct {
		int major;
		int minor;
	} width;
	int distance;	/* Hovering */
	int orientation;
	int pressure;
	int updated; /* Timestamp is updated */
	double tv;
	input_event_source_e source;
	double ratio_w;
	double ratio_h;
};

enum event_state {
	EVENT_STATE_DEACTIVATED,
	EVENT_STATE_ACTIVATE,
	EVENT_STATE_ACTIVATED,
	EVENT_STATE_DEACTIVATE,
	EVENT_STATE_ERROR
};

enum event_handler_activate_type {
	EVENT_HANDLER_DEACTIVATED = 0x00,
	EVENT_HANDLER_ACTIVATED_BY_MOUSE_SET = 0x01,
	EVENT_HANDLER_ACTIVATED_BY_SHOW = 0x02,
	EVENT_HANDLER_UNKNOWN = 0x04
};

extern int event_init(void);
extern int event_fini(void);
extern int event_input_fd(void);
extern int event_activate(int device, int x, int y, double ratio_w, double ratio_h, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data);
extern int event_deactivate(int device, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data);
extern int event_is_activated(void);
extern int event_reset_cbdata(int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data, void *new_data);

extern int event_deactivate_thread(enum event_handler_activate_type activate_type);
extern int event_activate_thread(enum event_handler_activate_type activate_type);

extern void event_set_mouse_xy(int device, int x, int y, double ratio_w, double ratio_h, double timestamp);

/* End of a file */
