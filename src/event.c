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

#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/input.h>

#include <Eina.h>
#include <Ecore.h>
#include <dlog.h>
#include <widget_errno.h>
#include <widget_service_internal.h>
#include <widget_conf.h>

#include "util.h"
#include "debug.h"
#include "conf.h"
#include "event.h"

#define EVENT_CH	'e'
#define EVENT_EXIT	'x'

#define PRESSURE 10
#define DELAY_COMPENSATOR 0.1f

#if !defined(EVIOCSCLOCKID)
/**
 * @note
 * I just copy this from the latest input.h file.
 * It could not be compatible with the latest one.
 */
#define EVIOCSCLOCKID           _IOW('E', 0xa0, int)                    /* Set clockid to be used for timestamps */
#endif

int errno;

static struct info {
	pthread_t tid;
	Eina_List *event_list;
	int handle;
	pthread_mutex_t event_list_lock;
	int evt_pipe[PIPE_MAX];
	int tcb_pipe[PIPE_MAX];
	Ecore_Fd_Handler *event_handler;

	struct event_data event_data;
	struct event_data skipped_event_data;

	Eina_List *event_listener_list;
	Eina_List *reactivate_list;

	enum event_handler_activate_type event_handler_activated;
	int timestamp_updated;
} s_info = {
	.event_handler_activated = EVENT_HANDLER_DEACTIVATED,
	.event_list = NULL,
	.handle = -1,
	.event_handler = NULL,
	.evt_pipe = { -1, -1 },
	.tcb_pipe = { -1, -1 },

	.event_data = {
		.x = -1,
		.y = -1,
		.device = -1,
		.slot = 0,
		.keycode = 0,
	},

	.skipped_event_data = {
		.x = -1,
		.y = -1,
		.device = -1,
		.slot = 0,
		.keycode = 0,
	},

	.event_listener_list = NULL,
	.reactivate_list = NULL,
	.timestamp_updated = 0,
};

struct event_listener {
	int (*event_cb)(enum event_state state, struct event_data *event, void *data);
	void *cbdata;

	enum event_state prev_state;
	enum event_state state;

	double tv;
	int x; /* RelX */
	int y; /* RelY */
	double ratio_w;
	double ratio_h;
	int slot;

	int unset_done;
};

static int event_control_fini(void);

HAPI int event_init(void)
{
	int ret;

	ret = pthread_mutex_init(&s_info.event_list_lock, NULL);
	if (ret != 0) {
		ErrPrint("Mutex: %d\n", ret);
		return WIDGET_ERROR_FAULT;
	}

	return WIDGET_ERROR_NONE;
}

HAPI int event_fini(void)
{
	int ret;

	event_control_fini();

	ret = pthread_mutex_destroy(&s_info.event_list_lock);
	if (ret != 0) {
		ErrPrint("Mutex destroy: %d\n", ret);
	}

	return WIDGET_ERROR_NONE;
}

/*
 * This function can be called Event Thread.
 */
static int push_event_item(void)
{
	struct event_data *item;

	if (s_info.event_data.x < 0 || s_info.event_data.y < 0) {
		/* Waiting full event packet */
		return WIDGET_ERROR_NONE;
	}

	item = malloc(sizeof(*item));
	if (item) {
		char event_ch = EVENT_CH;

		memcpy(item, &s_info.event_data, sizeof(*item));

		CRITICAL_SECTION_BEGIN(&s_info.event_list_lock);
		s_info.event_list = eina_list_append(s_info.event_list, item);
		CRITICAL_SECTION_END(&s_info.event_list_lock);

		if (write(s_info.evt_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
			ErrPrint("write: %d\n", errno);
			return WIDGET_ERROR_IO_ERROR;
		}

		/* Take a breathe */
		pthread_yield();
	} else {
		ErrPrint("malloc: %d\n", errno);
	}

	return WIDGET_ERROR_NONE;
}

static struct event_data *pop_event_item(void)
{
	struct event_data *item;

	CRITICAL_SECTION_BEGIN(&s_info.event_list_lock);
	item = eina_list_nth(s_info.event_list, 0);
	if (item) {
		s_info.event_list = eina_list_remove(s_info.event_list, item);
	}
	CRITICAL_SECTION_END(&s_info.event_list_lock);

	return item;
}

static double current_time_get(void)
{
	double ret;

	if (WIDGET_CONF_USE_GETTIMEOFDAY) {
		struct timeval tv;
		if (gettimeofday(&tv, NULL) < 0) {
			ErrPrint("gettimeofday: %d\n", errno);
			ret = ecore_time_get();
		} else {
			ret = (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0f);
		}
	} else {
		ret = ecore_time_get();
	}

	return ret;
}

static void update_timestamp(struct input_event *event)
{
	/*
	 * Input event uses timeval instead of timespec,
	 * but its value is same as MONOTIC CLOCK TIME
	 * So we should handles it properly.
	 */
	s_info.event_data.tv = (double)event->time.tv_sec + (double)event->time.tv_usec / 1000000.0f;
	s_info.timestamp_updated = 1;
}

static void processing_ev_abs(struct input_event *event)
{
	switch (event->code) {
#if defined(ABS_X)
	case ABS_X:
		break;
#endif
#if defined(ABS_Y)
	case ABS_Y:
		break;
#endif
#if defined(ABS_Z)
	case ABS_Z:
		break;
#endif
#if defined(ABS_RX)
	case ABS_RX:
		break;
#endif
#if defined(ABS_RY)
	case ABS_RY:
		break;
#endif
#if defined(ABS_RZ)
	case ABS_RZ:
		break;
#endif
#if defined(ABS_THROTTLE)
	case ABS_THROTTLE:
		break;
#endif
#if defined(ABS_RUDDER)
	case ABS_RUDDER:
		break;
#endif
#if defined(ABS_WHEEL)
	case ABS_WHEEL:
		break;
#endif
#if defined(ABS_GAS)
	case ABS_GAS:
		break;
#endif
#if defined(ABS_BRAKE)
	case ABS_BRAKE:
		break;
#endif
#if defined(ABS_HAT0X)
	case ABS_HAT0X:
		break;
#endif
#if defined(ABS_HAT0Y)
	case ABS_HAT0Y:
		break;
#endif
#if defined(ABS_HAT1X)
	case ABS_HAT1X:
		break;
#endif
#if defined(ABS_HAT1Y)
	case ABS_HAT1Y:
		break;
#endif
#if defined(ABS_HAT2X)
	case ABS_HAT2X:
		break;
#endif
#if defined(ABS_HAT2Y)
	case ABS_HAT2Y:
		break;
#endif
#if defined(ABS_HAT3X)
	case ABS_HAT3X:
		break;
#endif
#if defined(ABS_HAT3Y)
	case ABS_HAT3Y:
		break;
#endif
#if defined(ABS_PRESSURE)
	case ABS_PRESSURE:
		break;
#endif
#if defined(ABS_TILT_X)
	case ABS_TILT_X:
		break;
#endif
#if defined(ABS_TILT_Y)
	case ABS_TILT_Y:
		break;
#endif
#if defined(ABS_TOOL_WIDTH)
	case ABS_TOOL_WIDTH:
		break;
#endif
#if defined(ABS_VOLUME)
	case ABS_VOLUME:
		break;
#endif
#if defined(ABS_MISC)
	case ABS_MISC:
		break;
#endif
#if defined(ABS_DISTANCE)
	case ABS_DISTANCE:
		s_info.event_data.distance = event->value;
		break;
#endif
#if defined(ABS_MT_POSITION_X)
	case ABS_MT_POSITION_X:
		s_info.event_data.x = event->value;
		break;
#endif
#if defined(ABS_MT_POSITION_Y)
	case ABS_MT_POSITION_Y:
		s_info.event_data.y = event->value;
		break;
#endif
#if defined(ABS_MT_SLOT)
	case ABS_MT_SLOT:
		s_info.event_data.slot = event->value;
		break;
#endif
#if defined(ABS_MT_TRACKING_ID)
	case ABS_MT_TRACKING_ID:
		s_info.event_data.device = event->value;
		break;
#endif
#if defined(ABS_MT_TOUCH_MAJOR)
	case ABS_MT_TOUCH_MAJOR:
		s_info.event_data.touch.major = event->value;
		break;
#endif
#if defined(ABS_MT_TOUCH_MINOR)
	case ABS_MT_TOUCH_MINOR:
		s_info.event_data.touch.minor = event->value;
		break;
#endif
#if defined(ABS_MT_WIDTH_MAJOR)
	case ABS_MT_WIDTH_MAJOR:
		s_info.event_data.width.major = event->value;
		break;
#endif
#if defined(ABS_MT_WIDTH_MINOR)
	case ABS_MT_WIDTH_MINOR:
		s_info.event_data.width.minor = event->value;
		break;
#endif
#if defined(ABS_MT_ORIENTATION)
	case ABS_MT_ORIENTATION:
		s_info.event_data.orientation = event->value;
		break;
#endif
#if defined(ABS_MT_PRESSURE)
	case ABS_MT_PRESSURE:
		s_info.event_data.pressure = event->value;
		break;
#endif
#if defined(ABS_MT_TOOL_X)
	case ABS_MT_TOOL_X:
		DbgPrint("TOOL_X: %d\n", event->value);
		break;
#endif
#if defined(ABS_MT_TOOL_Y)
	case ABS_MT_TOOL_Y:
		DbgPrint("TOOL_Y: %d\n", event->value);
		break;
#endif
#if defined(ABS_MT_TOOL_TYPE)
	case ABS_MT_TOOL_TYPE:
		DbgPrint("TOOL_TYPE: %d\n", event->value);
		break;
#endif
#if defined(ABS_MT_BLOB_ID)
	case ABS_MT_BLOB_ID:
		DbgPrint("BLOB_ID: %d\n", event->value);
		break;
#endif
#if defined(ABS_MT_DISTANCE)
	case ABS_MT_DISTANCE:
		DbgPrint("DISTANCE: %d\n", event->value);
		break;
#endif
#if defined(ABS_MT_PALM)
	case ABS_MT_PALM:
		DbgPrint("PALM: %d\n", event->value);
		break;
#endif
	default:
#if defined(ABS_MT_COMPONENT)
		if (event->code == ABS_MT_COMPONENT) {
			DbgPrint("COMPONENT: %d\n", event->value);
			break;
		}
#endif
#if defined(ABS_MT_ANGLE)
		if (event->code == ABS_MT_ANGLE) {
			DbgPrint("ANGLE: %d\n", event->value);
			break;
		}
#endif
#if defined(ABS_MT_SUMSIZE)
		if (event->code == ABS_MT_SUMSIZE) {
			DbgPrint("SUMSIZE: %d\n", event->value);
			break;
		}
#endif
		break;
	}

	return;
}

/*
 * Called by Event Thread
 */
static inline int processing_input_event(struct input_event *event)
{
	int ret;

	if (s_info.timestamp_updated == 0) {
		update_timestamp(event);
	}

	switch (event->type) {
	case EV_SYN:
		switch (event->code) {
		case SYN_CONFIG:
			break;
		case SYN_MT_REPORT:
		case SYN_REPORT:
			s_info.timestamp_updated = 0;
			ret = push_event_item();
			if (ret < 0) {
				return ret;
			}

			break;
#if defined(SYN_DROPPED)
		case SYN_DROPPED:
			DbgPrint("EV_SYN, SYN_DROPPED\n");
			break;
#endif
		default:
			DbgPrint("EV_SYN, 0x%x\n", event->code);
			break;
		}
		break;
	case EV_KEY:
		DbgPrint("EV_KEY: 0x%X\n", event->value);
		s_info.event_data.keycode = event->value;
		s_info.event_data.source = INPUT_EVENT_SOURCE_NODE;
		break;
	case EV_REL:
		DbgPrint("EV_REL: 0x%X\n", event->value);
		break;
	case EV_ABS:
		processing_ev_abs(event);
		s_info.event_data.source = INPUT_EVENT_SOURCE_NODE;
		break;
	case EV_MSC:
	case EV_SW:
	case EV_LED:
	case EV_SND:
	case EV_REP:
	case EV_FF:
	case EV_PWR:
	case EV_FF_STATUS:
	default:
		DbgPrint("0x%X, 0x%X\n", event->type, event->code);
		break;
	}

	return WIDGET_ERROR_NONE;
}

static void *event_thread_main(void *data)
{
	fd_set set;
	long ret = 0;
	struct input_event input_event;
	char *ptr = (char *)&input_event;
	int offset = 0;
	int readsize = 0;
	int fd;
	char event_ch;

	s_info.event_data.x = -1;
	s_info.event_data.y = -1;
	s_info.event_data.slot = 0;

	while (1) {
		FD_ZERO(&set);
		FD_SET(s_info.handle, &set);
		FD_SET(s_info.tcb_pipe[PIPE_READ], &set);

		fd = s_info.handle > s_info.tcb_pipe[PIPE_READ] ? s_info.handle : s_info.tcb_pipe[PIPE_READ];
		ret = select(fd + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				DbgPrint("Select receives INTR\n");
				continue;
			}
			ErrPrint("select: %d\n", errno);
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout expired\n");
			ret = WIDGET_ERROR_TIMED_OUT;
			break;
		}

		if (FD_ISSET(s_info.handle, &set)) {
			readsize = read(s_info.handle, ptr + offset, sizeof(input_event) - offset);
			if (readsize < 0) {
				ErrPrint("read: %d / fd: %d / offset: %d / size: %d - %d\n", errno, s_info.handle, offset, sizeof(input_event), readsize);
				ret = WIDGET_ERROR_FAULT;
				break;
			}

			offset += readsize;
			if (offset == sizeof(input_event)) {
				offset = 0;
				if (processing_input_event(&input_event) < 0) {
					ret = WIDGET_ERROR_FAULT;
					break;
				}
			}

			/*
			 * If there is input event,
			 * Try again to get the input event.
			 */
		} else if (!s_info.timestamp_updated && FD_ISSET(s_info.tcb_pipe[PIPE_READ], &set)) {
			/*!
			 * Check the s_info.timestamp_updated flag.
			 * If it is not ZERO, it means, there is event to be processed.
			 * So we have to wait it to be finished.
			 */
			if (read(s_info.tcb_pipe[PIPE_READ], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
				ErrPrint("read: %d\n", errno);
			}

			ret = WIDGET_ERROR_CANCELED;
			break;
		}
	}

	event_ch = EVENT_EXIT;
	if (write(s_info.evt_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
		ErrPrint("write: %d\n", errno);
	}

	return (void *)ret;
}

static int invoke_event_cb(struct event_listener *listener, struct event_data *item)
{
	struct event_data modified_item;

	memcpy(&modified_item, item, sizeof(modified_item));

	modified_item.x -= listener->x;
	modified_item.y -= listener->y;
	modified_item.ratio_w = listener->ratio_w;
	modified_item.ratio_h = listener->ratio_h;

	if (!WIDGET_CONF_USE_EVENT_TIME) {
		item->tv = current_time_get();
	}

	if (listener->event_cb(listener->state, &modified_item, listener->cbdata) < 0) {
		if (eina_list_data_find(s_info.event_listener_list, listener)) {
			s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
			DbgFree(listener);
			return 1;
		}
	}

	return 0;
}

static inline void clear_listener(struct event_listener *listener)
{
	enum event_state next_state;
	struct event_data event_data;
	struct event_data *p_event_data;

	/**
	 * @note
	 * terminate this listener. with keep its event state (DOWN -> MOVE -> UP)
	 */
	while (listener) {
		DbgPrint("listener[%p] prev[%x] state[%x]\n", listener, listener->prev_state, listener->state);

		switch (listener->state) {
		case EVENT_STATE_ACTIVATE:
			p_event_data = &s_info.event_data;
			next_state = EVENT_STATE_ACTIVATED;
			break;
		case EVENT_STATE_ACTIVATED:
			p_event_data = &s_info.event_data;
			next_state = EVENT_STATE_DEACTIVATE;
			break;
		case EVENT_STATE_DEACTIVATE:
			memcpy(&event_data, &s_info.event_data, sizeof(event_data));
			p_event_data = &event_data;

			if (listener->prev_state == EVENT_STATE_ACTIVATE) {
				/* There is no move event. we have to emulate it */
				DbgPrint ("Let's emulate move event (%dx%d)\n", p_event_data->x, p_event_data->y);
				listener->state = EVENT_STATE_ACTIVATE;
				next_state = EVENT_STATE_ACTIVATED;
			} else {
				next_state = EVENT_STATE_DEACTIVATED;
			}
			break;
		case EVENT_STATE_DEACTIVATED:
		default:
			s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
			DbgFree(listener);
			listener = NULL;
			continue;
		}

		p_event_data->slot = listener->slot;
		if (invoke_event_cb(listener, p_event_data)) {
			/**
			 * @note
			 * listener is deleted.
			 */
			listener = NULL;
			continue;
		}

		/*!
		 * Changing state of listener will affect to the event collecting thread.
		 */
		listener->prev_state = listener->state;
		listener->state = next_state;
	}
}

static inline void clear_all_listener_list(void)
{
	struct event_listener *listener;
	Eina_List *l;
	Eina_List *n;

	DbgPrint("event listeners: %d\n", eina_list_count(s_info.event_listener_list));

	if (s_info.event_data.x == -1 || s_info.event_data.y == -1) {
		memcpy(&s_info.event_data, &s_info.skipped_event_data, sizeof(s_info.event_data));
		DbgPrint("Use skipped event: %dx%d\n", s_info.skipped_event_data.x, s_info.skipped_event_data.y);
	}

	EINA_LIST_FOREACH_SAFE(s_info.event_listener_list, l, n, listener) {
		clear_listener(listener);
	}
}

static int compare_timestamp(struct event_listener *listener, struct event_data *item)
{
	int ret;
	if (listener->tv > item->tv) {
		ret = 1;
	} else if (listener->tv < item->tv) {
		ret = -1;
	} else {
		ret = 0;
	}
	return ret;
}

static Eina_Bool event_read_cb(void *data, Ecore_Fd_Handler *handler)
{
	int fd;
	struct event_data *item;
	char event_ch;
	struct event_listener *listener;
	Eina_List *l;
	Eina_List *n;
	enum event_state next_state;
	int state;

	fd = ecore_main_fd_handler_fd_get(handler);
	if (fd < 0) {
		ErrPrint("Invalid fd\n");
		return ECORE_CALLBACK_CANCEL;
	}

	if (read(fd, &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
		ErrPrint("read: %d\n", errno);
		return ECORE_CALLBACK_CANCEL;
	}

	if (event_ch == EVENT_EXIT) {
		/*!
		 * If the master gets event exit from evt_pipe,
		 * The event item list should be empty.
		 */
		if (!s_info.event_list) {
			/* This callback must has to clear all listeners in this case */
			ecore_main_fd_handler_del(s_info.event_handler);
			s_info.event_handler = NULL;
			clear_all_listener_list();

			EINA_LIST_FREE(s_info.reactivate_list, listener) {
				s_info.event_listener_list = eina_list_append(s_info.event_listener_list, listener);
			}
			DbgPrint("Reactivate: %p\n", s_info.event_listener_list);

			if (s_info.event_listener_list) {
				if (event_activate_thread(EVENT_HANDLER_ACTIVATED_BY_MOUSE_SET) < 0) {
					EINA_LIST_FREE(s_info.event_listener_list, listener) {
						(void)listener->event_cb(EVENT_STATE_ERROR, NULL, listener->cbdata);
					}
				}
			}

			DbgPrint("Event read callback finshed (%p)\n", s_info.event_listener_list);
			return ECORE_CALLBACK_CANCEL;
		} else {
			ErrPrint("Something goes wrong, the event_list is not flushed\n");
		}
	}

	item = pop_event_item();
	if (!item) {
		ErrPrint("There is no remained event\n");
		return ECORE_CALLBACK_RENEW;
	}

	EINA_LIST_FOREACH_SAFE(s_info.event_listener_list, l, n, listener) {
		if (item->slot != listener->slot) {
			continue;
		}

		if (item->device == -1) {
			DbgPrint("Touch released (%d) listener state(%x)\n", item->slot, listener->state);
		}

		switch (listener->state) {
		case EVENT_STATE_ACTIVATE:
			if (compare_timestamp(listener, item) > 0) {
				memcpy(&s_info.skipped_event_data, item, sizeof(s_info.skipped_event_data));
				continue;
			}

			next_state = EVENT_STATE_ACTIVATED;
			break;
		case EVENT_STATE_DEACTIVATE:
			if (compare_timestamp(listener, item) < 0) {
				if (listener->unset_done == 1) {
					/**
					 * @note
					 * in case of the multi=touch,
					 * The thread will not be terminated soon.
					 * in this case, we should send UP event and destroy listener properly.
					 */
					clear_listener(listener);
					/**
					 * @note
					 * Listener will be deleted.
					 * But event_item which is related with this listener can be remained.
					 * And it will be deleted soon.
					 */
					continue;
				}

				/* Consuming all events occurred while activating this listener */
				state = listener->prev_state;
				listener->prev_state = listener->state;

				if (state == EVENT_STATE_ACTIVATE) {
					/**
					 * @note
					 * If the previous state is ACTIVATE
					 * We should create DOWN event first.
					 */
					listener->state = EVENT_STATE_ACTIVATE;
				} else {
					listener->state = EVENT_STATE_ACTIVATED;
				}

				if (invoke_event_cb(listener, item) == 1) {
					/**
					 * @note
					 * listener is deleted
					 */
					continue;
				}

				if (listener->state == EVENT_STATE_ACTIVATE) {
					/**
					 * @note
					 * We are already jumped into the DEACTIVATE state
					 * But the state is ACTIVATE, it means, we forcely chnaged it from LINE 747
					 * We successfully sent the DOWN event.
					 * We only need to send UP (or move) event.
					 * So change the previous states to ACTIVATED and then change current states to
					 * DEACTIVATE again.
					 */
					listener->prev_state = EVENT_STATE_ACTIVATED;
				} else {
					listener->prev_state = listener->state;
				}

				listener->state = EVENT_STATE_DEACTIVATE;
			} else {
				memcpy(&s_info.skipped_event_data, item, sizeof(s_info.skipped_event_data));
			}

			/**
			 * @note
			 * Do not terminate this listener, until this mets EVENT_EXIT
			 */
			continue;
		case EVENT_STATE_ACTIVATED:
			if (compare_timestamp(listener, item) > 0) {
				DbgPrint("Drop event (%lf > %lf)\n", listener->tv, item->tv);
				memcpy(&s_info.skipped_event_data, item, sizeof(s_info.skipped_event_data));
				continue;
			}
			next_state = listener->state;
			break;
		case EVENT_STATE_DEACTIVATED:
		default:
			/* Remove this from the list */
			/* Check the item again. the listener can be deleted from the callback */
			if (eina_list_data_find(s_info.event_listener_list, listener)) {
				s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
				DbgFree(listener);
			}

			continue;
		}

		if (invoke_event_cb(listener, item) == 1) {
			/**
			 * @note
			 * listener is deleted.
			 */
			continue;
		}

		listener->prev_state = listener->state;
		listener->state = next_state;
	}

	DbgFree(item);

	return ECORE_CALLBACK_RENEW;
}

static int event_control_init(void)
{
	int status;
	unsigned int clockId = CLOCK_MONOTONIC;

	DbgPrint("Initializing event controller\n");
	if (s_info.handle != -1) {
		return WIDGET_ERROR_NONE;
	}

	s_info.handle = open(WIDGET_CONF_INPUT_PATH, O_RDONLY);
	if (s_info.handle < 0) {
		ErrPrint("open: %d\n", errno);
		return WIDGET_ERROR_IO_ERROR;
	}

	if (fcntl(s_info.handle, F_SETFD, FD_CLOEXEC) < 0) {
		ErrPrint("fcntl: %d\n", errno);
	}

	if (fcntl(s_info.handle, F_SETFL, O_NONBLOCK) < 0) {
		ErrPrint("fcntl: %d\n", errno);
	}

	if (WIDGET_CONF_USE_EVENT_TIME && !WIDGET_CONF_USE_GETTIMEOFDAY) {
		DbgPrint("Change timestamp to monotonic\n");
		if (ioctl(s_info.handle, EVIOCSCLOCKID, &clockId) < 0) {
			ErrPrint("ioctl: %d\n", errno);
		}
	}

	status = pipe2(s_info.evt_pipe, O_CLOEXEC);
	if (status < 0) {
		ErrPrint("pipe2: %d\n", errno);
		if (close(s_info.handle) < 0) {
			ErrPrint("close: %d\n", errno);
		}
		s_info.handle = -1;
		return WIDGET_ERROR_FAULT;
	}

	status = pipe2(s_info.tcb_pipe, O_CLOEXEC);
	if (status < 0) {
		ErrPrint("pipe2: %d\n", errno);
		if (close(s_info.handle) < 0) {
			ErrPrint("close: %d\n", errno);
		}
		s_info.handle = -1;
		CLOSE_PIPE(s_info.evt_pipe);
		return WIDGET_ERROR_FAULT;
	}

	return WIDGET_ERROR_NONE;
}

/*!
 * This function must has to be called after event collecting thread is terminated
 */
static int event_control_fini(void)
{
	DbgPrint("Finalizing event controller\n");
	if (s_info.handle != -1) {
		if (close(s_info.handle) < 0) {
			ErrPrint("close: %d\n", errno);
		}

		s_info.handle = -1;
	}

	if (!eina_list_count(s_info.event_list)) {
		if (s_info.event_handler) {
			ecore_main_fd_handler_del(s_info.event_handler);
			s_info.event_handler = NULL;
		}
		clear_all_listener_list();
	}

	CLOSE_PIPE(s_info.tcb_pipe);
	CLOSE_PIPE(s_info.evt_pipe);

	return WIDGET_ERROR_NONE;
}

int event_activate_thread(enum event_handler_activate_type activate_type)
{
	int ret;

	ret = event_control_init();
	if (ret != WIDGET_ERROR_NONE) {
		return ret;
	}

	if (s_info.event_handler) {
		ErrPrint("Event handler is already registered\n");
		return WIDGET_ERROR_ALREADY_EXIST;
	}

	s_info.event_handler = ecore_main_fd_handler_add(s_info.evt_pipe[PIPE_READ], ECORE_FD_READ, event_read_cb, NULL, NULL, NULL);
	if (!s_info.event_handler) {
		ErrPrint("Failed to add monitor for EVT READ\n");
		return WIDGET_ERROR_FAULT;
	}

	ret = pthread_create(&s_info.tid, NULL, event_thread_main, NULL);
	if (ret != 0) {
		ErrPrint("pthread_create: %d\n", ret);
		ecore_main_fd_handler_del(s_info.event_handler);
		s_info.event_handler = NULL;
		return WIDGET_ERROR_FAULT;
	}

	DbgPrint("Event handler activated\n");
	s_info.event_handler_activated = activate_type;
	return WIDGET_ERROR_NONE;
}

int event_deactivate_thread(enum event_handler_activate_type activate_type)
{
	int status;
	void *ret;
	char event_ch = EVENT_CH;

	if (s_info.event_handler_activated != activate_type) {
		ErrPrint("Event handler activate type is mismatched\n");
		return WIDGET_ERROR_INVALID_PARAMETER;
	}

	/* Terminating thread */
	if (write(s_info.tcb_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
		ErrPrint("write: %d\n", errno);
	}

	status = pthread_join(s_info.tid, &ret);
	if (status != 0) {
		ErrPrint("pthread_join: %d\n", status);
	} else {
		DbgPrint("Thread returns: %p\n", ret);
	}

	s_info.event_handler_activated = EVENT_HANDLER_DEACTIVATED;
	return WIDGET_ERROR_NONE;
}

/*!
 * x, y is the starting point.
 */
HAPI int event_activate(int slot, int x, int y, double ratio_w, double ratio_h, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data)
{
	struct event_listener *listener;
	int ret = WIDGET_ERROR_NONE;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.event_listener_list, l, listener) {
		if (listener->event_cb == event_cb && listener->cbdata == data && listener->slot == slot) {
			ErrPrint("Already registered\n");
			return WIDGET_ERROR_ALREADY_EXIST;
		}
	}

	listener = malloc(sizeof(*listener));
	if (!listener) {
		ErrPrint("malloc: %d\n", errno);
		return WIDGET_ERROR_OUT_OF_MEMORY;
	}

	listener->tv = current_time_get() - DELAY_COMPENSATOR; // Let's use the previous event.
	DbgPrint("Activated at: %lf (%dx%d)\n", listener->tv, x, y);

	listener->event_cb = event_cb;
	listener->cbdata = data;
	listener->prev_state = EVENT_STATE_DEACTIVATED;
	listener->state = EVENT_STATE_ACTIVATE;
	listener->x = x;
	listener->y = y;
	listener->ratio_w = ratio_w;
	listener->ratio_h = ratio_h;
	listener->slot = slot;
	listener->unset_done = 0;

	if (s_info.event_handler_activated == EVENT_HANDLER_DEACTIVATED) {
		/*!
		 * \note
		 * We don't need to lock to access event_list here.
		 * If the _sinfo.handle is greater than 0, the event_list will not be touched.
		 * But if the s_info.handle is less than 0, it means, there is no thread,
		 * so we can access the event_list without lock.
		 */
		if (s_info.event_list) {
			DbgPrint("Event thread is deactivating now. activating will be delayed\n");
			s_info.reactivate_list = eina_list_append(s_info.reactivate_list, listener);
		} else {
			s_info.event_listener_list = eina_list_append(s_info.event_listener_list, listener);

			if ((ret = event_activate_thread(EVENT_HANDLER_ACTIVATED_BY_MOUSE_SET)) < 0) {
				s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
				DbgFree(listener);
			}
		}
	} else {
		s_info.event_listener_list = eina_list_append(s_info.event_listener_list, listener);
	}

	return ret;
}

HAPI int event_input_fd(void)
{
	event_control_init();
	DbgPrint("Input event handler: %d\n", s_info.handle);
	return s_info.handle;
}

HAPI int event_deactivate(int slot, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data)
{
	struct event_listener *listener = NULL;
	struct event_listener *item;
	Eina_List *l;
	int keep_thread = 0;

	EINA_LIST_FOREACH(s_info.event_listener_list, l, item) {
		if (item->event_cb == event_cb && item->cbdata == data && (slot < 0 || item->slot == slot)) {
			switch (item->state) {
			case EVENT_STATE_ACTIVATE:
			case EVENT_STATE_ACTIVATED:
				item->prev_state = item->state;
				item->state = EVENT_STATE_DEACTIVATE;
				listener = item;
				break;
			default:
				/* Item is already deactivated */
				break;
			}
		}

		keep_thread += (item->state == EVENT_STATE_ACTIVATE || item->state == EVENT_STATE_ACTIVATED);
	}

	if (!listener) {
		ErrPrint("Listener is not registered or already deactivated\n");
		return WIDGET_ERROR_NOT_EXIST;
	}

	if (s_info.event_handler_activated == EVENT_HANDLER_DEACTIVATED) {
		ErrPrint("Event handler is not actiavated\n");
		s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
		DbgFree(listener);
		return WIDGET_ERROR_NONE;
	}

	if (keep_thread) {
		DbgPrint("Keep thread\n");
		listener->unset_done = 1;

		/**
		 * @note
		 * This will invoke the event_read_cb callback with fake event data.
		 * At that time we should terminate this listener.
		 */
		push_event_item();
		return WIDGET_ERROR_NONE;
	}

	event_deactivate_thread(EVENT_HANDLER_ACTIVATED_BY_MOUSE_SET);

	return WIDGET_ERROR_NONE;
}

HAPI int event_reset_cbdata(int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data, void *new_data)
{
	struct event_listener *item;
	Eina_List *l;
	int updated = 0;

	EINA_LIST_FOREACH(s_info.event_listener_list, l, item) {
		if (item->event_cb == event_cb && item->cbdata == data) {
			item->cbdata = new_data;
			updated++;
		}
	}

	EINA_LIST_FOREACH(s_info.reactivate_list, l, item) {
		if (item->event_cb == event_cb && item->cbdata == data) {
			item->cbdata = new_data;
			updated++;
		}
	}

	return updated;
}

HAPI int event_is_activated(void)
{
	return s_info.handle >= 0;
}

HAPI void event_set_mouse_xy(int slot, int x, int y, double ratio_w, double ratio_h, double timestamp)
{
	s_info.event_data.x = x;
	s_info.event_data.y = y;
	s_info.event_data.ratio_w = ratio_w;
	s_info.event_data.ratio_h = ratio_h;
	s_info.event_data.tv = timestamp;
	s_info.event_data.source = INPUT_EVENT_SOURCE_VIEWER;
	s_info.event_data.slot = slot;
	/**
	 * Don't touch the timestamp_updated variable.
	 * if we toggle it, the input thread will not be terminated correctly. SEE LINE: 537
	if (!s_info.timestamp_updated) {
		// NOP
	}
	 */
}

/* End of a file */
