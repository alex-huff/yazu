#include "yazu.h"
#include "buffer.h"
#include "pixfmt.h"

// milliseconds
#define SAMPLE_IS_OLD_THRESHOLD 50

#define wl_array_add_item(array, item, is_first) \
{ \
	struct wl_array *_array = array; \
	size_t item_size = sizeof(*item); \
	size_t old_num_items = _array->size / item_size; \
	wl_array_add(_array, item_size); \
	item = _array->data; \
	if (is_first) memmove(item + 1, item, old_num_items * item_size); \
	else item += old_num_items; \
}
#define wl_array_add_first(wl_array, item) wl_array_add_item(wl_array, item, true);
#define wl_array_add_last(wl_array, item) wl_array_add_item(wl_array, item, false);

#define wl_array_remove(array, removed_item, index) \
{ \
	struct wl_array *_array = array; \
	size_t _index = index; \
	size_t item_size = sizeof(removed_item); \
	size_t old_num_items = _array->size / item_size; \
	assert(_index >= 0 && _index < old_num_items); \
	size_t removed_byte_index = item_size * _index; \
	void *removed_data = _array->data + removed_byte_index; \
	memcpy(&removed_item, removed_data, item_size); \
	memmove(removed_data, removed_data + item_size, \
		(old_num_items - (_index + 1)) * item_size); \
	_array->size -= item_size; \
}

static const char *vertex_shader_string =
	"attribute vec2 vertex_position;\n"
	"attribute vec2 texture_position;\n"
	"varying vec2 v_texture_position;\n"
	"void main() {\n"
	"	gl_Position = vec4(vertex_position, 0.0, 1.0);\n"
	"	v_texture_position = texture_position;\n"
	"}\n";
static const char *fragment_shader_string =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D capture_texture;\n"
	"varying vec2 v_texture_position;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(capture_texture, v_texture_position);\n"
	"}\n";

static void setup_surface_frame_callback(struct yazu *yazu);
static void setup_viewport_source(struct yazu *yazu, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height);
static void set_dirty(struct yazu *yazu);
static void send_frame(struct yazu *yazu);
static double real_zoom_scale(struct yazu *yazu);
static double buffer_x_to_capture_x(struct yazu *yazu, double buffer_x);
static double buffer_y_to_capture_y(struct yazu *yazu, double buffer_y);
static bool clamp_capture_target_x(struct yazu *yazu);
static bool clamp_capture_target_y(struct yazu *yazu);
static bool clamp_capture_target(struct yazu *yazu);
static void drag_capture(struct yazu *yazu,
		double old_buffer_x, double old_buffer_y,
		double buffer_x, double buffer_y);
static void recompute_dimensions(struct yazu *yazu);
static void trim_old_motion_events(struct wl_array *motion_events,
		uint32_t time);
static void add_motion_event(struct wl_array *motion_events, double x,
		double y, uint32_t time);
static void handle_drag_release(struct yazu* yazu,
		struct wl_array *motion_events, uint32_t time);
static double squared_distance(double dx, double dy);
static void process_animations(struct yazu *yazu, uint32_t time);
static void destroy_pointer(struct yazu_seat *yazu_seat);
static void destroy_touch(struct yazu_seat *yazu_seat);

// BEGIN POINTER

static void pointer_set_shape(struct yazu_seat *seat,
		struct wl_pointer *wl_pointer, uint32_t serial) {
	enum wp_cursor_shape_device_v1_shape shape =
		seat->pointer_dragging ?
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING :
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
	wp_cursor_shape_device_v1_set_shape(seat->wp_cursor_shape_device,
		serial, shape);
}

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct yazu_seat *seat = data;
	assert(!seat->pointer_on_surface);
	seat->pointer_on_surface = true;
	pointer_set_shape(seat, wl_pointer, serial);
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct yazu_seat *seat = data;
	assert(seat->pointer_on_surface);
	seat->pointer_on_surface = false;
}

static double surface_xy_to_buffer_x(struct yazu *yazu, double surface_x, double surface_y) {
	switch (yazu->transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return surface_x * yazu->buffer_scale_x;

	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		return (yazu->width - surface_x) * yazu->buffer_scale_x;

	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		return surface_y * yazu->buffer_scale_y;

	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return (yazu->height - surface_y) * yazu->buffer_scale_y;
	}

	assert(false);
}

static double surface_xy_to_buffer_y(struct yazu *yazu, double surface_x, double surface_y) {
	switch (yazu->transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		return surface_y * yazu->buffer_scale_y;

	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return (yazu->height - surface_y) * yazu->buffer_scale_y;

	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return (yazu->width - surface_x) * yazu->buffer_scale_x;

	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		return surface_x * yazu->buffer_scale_x;
	}

	assert(false);
}

static void get_buffer_xy_from_surface_xy(struct yazu *yazu,
		double surface_x, double surface_y,
		double *buffer_x, double *buffer_y) {
	*buffer_x = surface_xy_to_buffer_x(yazu, surface_x, surface_y);
	*buffer_y = surface_xy_to_buffer_y(yazu, surface_x, surface_y);
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	assert(seat->pointer_on_surface);
	if (!yazu->capture.frame_ready) {
		return;
	}

	double old_cursor_x = seat->cursor_x;
	double old_cursor_y = seat->cursor_y;
	get_buffer_xy_from_surface_xy(yazu,
		wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y),
		&seat->cursor_x, &seat->cursor_y);

	if (seat->pointer_dragging) {
		drag_capture(yazu, old_cursor_x, old_cursor_y, seat->cursor_x, seat->cursor_y);
	}

	add_motion_event(&seat->pointer_motion_events,
		seat->cursor_x, seat->cursor_y, time);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button,
		uint32_t button_state) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	assert(seat->pointer_on_surface);
	if (!yazu->capture.frame_ready) {
		return;
	}

	seat->button_state = button_state;
	seat->last_button = button;
	switch (button) {
	case BTN_LEFT:
		bool is_pressed = button_state == WL_POINTER_BUTTON_STATE_PRESSED;
		if (!yazu->dragging && is_pressed) {
			yazu->sliding = false;
			yazu->dragging = true;
			seat->pointer_dragging = true;

			pointer_set_shape(seat, wl_pointer, serial);
		} else if (seat->pointer_dragging && !is_pressed) {
			yazu->dragging = false;
			seat->pointer_dragging = false;

			handle_drag_release(yazu, &seat->pointer_motion_events,
				time);
			pointer_set_shape(seat, wl_pointer, serial);
		}

		break;
	case BTN_RIGHT:
		yazu->running = false;

		break;
	case BTN_MIDDLE:
	default:
		break;
	}
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t fixed_value) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	assert(seat->pointer_on_surface);
	if (!yazu->capture.frame_ready) {
		return;
	}

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}

	double value = wl_fixed_to_double(fixed_value);
	double old_zoom_target_percent = yazu->zoom_target_percent;
	yazu->zoom_target_percent -= 2 * value;
	yazu->zoom_target_percent = MAX(100, yazu->zoom_target_percent);
	if (yazu->zoom_target_percent == old_zoom_target_percent) {
		return;
	}

	yazu->zoom_seat = seat;

	if (yazu->zooming) {
		return;
	}

	yazu->zooming = true;
	yazu->zoom_last_tick_time = time;

	set_dirty(yazu);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_handle_enter,
	.leave = pointer_handle_leave,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = pointer_handle_axis,
};

// END POINTER

// BEGIN TOUCH

static size_t get_num_touch_points(struct yazu_seat *seat) {
	return seat->touch_points.size / sizeof(struct yazu_touch_point);
}

static struct yazu_touch_point * get_touch_point(struct yazu_seat *seat,
		int32_t id, int32_t *index) {
	struct yazu_touch_point *point;
	int32_t i = 0;
	wl_array_for_each(point, &seat->touch_points) {
		if (point->id == id) {
			if (index != NULL) {
				*index = i;
			}

			return point;
		}

		i++;
	}

	if (index != NULL) {
		*index = -1;
	}

	return NULL;
}

static void touch_handle_down(struct yazu_seat *seat, int32_t id,
		uint32_t time, double x, double y) {
	struct yazu *yazu = seat->yazu;
	struct yazu_touch_point *touch_point = get_touch_point(seat, id, NULL);
	assert(touch_point == NULL);

	wl_array_add_last(&seat->touch_points, touch_point);
	touch_point->id = id;
	wl_array_init(&touch_point->motion_events);
	add_motion_event(&touch_point->motion_events, x, y, time);

	if (!yazu->dragging && get_num_touch_points(seat) == 1) {
		yazu->sliding = false;
		yazu->dragging = true;
		seat->touch_dragging = true;
	}
}

static void touch_handle_up(struct yazu_seat *seat, int32_t id,
		uint32_t time) {
	struct yazu *yazu = seat->yazu;
	int32_t touch_point_index;
	struct yazu_touch_point *touch_point = get_touch_point(seat, id,
		&touch_point_index);
	assert(touch_point != NULL);

	struct yazu_touch_point removed_touch_point;
	wl_array_remove(&seat->touch_points, removed_touch_point,
		touch_point_index);
	uint32_t num_touch_points = get_num_touch_points(seat);

	if (num_touch_points == 0 && seat->touch_dragging) {
		yazu->dragging = false;
		seat->touch_dragging = false;

		handle_drag_release(yazu, &removed_touch_point.motion_events,
			time);
	}

	wl_array_release(&removed_touch_point.motion_events);
}

static void touch_handle_motion(struct yazu_seat *seat, int32_t id,
		uint32_t time, double x, double y) {
	struct yazu_touch_point *touch_point = get_touch_point(seat, id, NULL);
	assert(touch_point != NULL);

	uint32_t num_touch_points = get_num_touch_points(seat);
	if (seat->touch_dragging && num_touch_points == 1) {
		struct yazu_input_motion_event *last_input_motion_event =
			touch_point->motion_events.data;
		drag_capture(seat->yazu,
			last_input_motion_event->x, last_input_motion_event->y,
			x, y);
	}

	add_motion_event(&touch_point->motion_events, x, y, time);
}

static void touch_queue_event(struct yazu_seat *seat,
		enum yazu_touch_event_type type, int32_t id, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	struct yazu_touch_event *touch_event;
	wl_array_add_last(&seat->wl_touch_events, touch_event);
	touch_event->type = type;
	touch_event->id = id;
	touch_event->time = time;
	get_buffer_xy_from_surface_xy(seat->yazu,
		wl_fixed_to_double(x), wl_fixed_to_double(y),
		&touch_event->x, &touch_event->y);
}

static void touch_queue_handle_down(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, struct wl_surface *wl_surface,
		int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct yazu_seat *seat = data;
	touch_queue_event(seat, DOWN, id, time, x, y);
}

static void touch_queue_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id) {
	struct yazu_seat *seat = data;
	touch_queue_event(seat, UP, id, time,
		wl_fixed_from_int(0), wl_fixed_from_int(0));
}

static void touch_queue_handle_motion(void *data, struct wl_touch *wl_touch,
		uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct yazu_seat *seat = data;
	touch_queue_event(seat, MOTION, id, time, x, y);
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch) {
	struct yazu_seat *seat = data;

	struct yazu_touch_event *event;
	wl_array_for_each(event, &seat->wl_touch_events) {
		switch(event->type) {
		case DOWN:
			touch_handle_down(seat, event->id, event->time, event->x, event->y);

			break;
		case UP:
			touch_handle_up(seat, event->id, event->time);

			break;
		case MOTION:
			touch_handle_motion(seat, event->id, event->time, event->x, event->y);

			break;
		}
	}

	seat->wl_touch_events.size = 0;
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	if (seat->touch_dragging) {
		yazu->dragging = false;
		seat->touch_dragging = false;
	}

	struct wl_array *touch_points = &seat->touch_points;
	struct yazu_touch_point *touch_point;
	wl_array_for_each(touch_point, touch_points) {
		wl_array_release(&touch_point->motion_events);
	}
	touch_points->size = 0;

	seat->wl_touch_events.size = 0;
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_queue_handle_down,
	.up = touch_queue_handle_up,
	.motion = touch_queue_handle_motion,
	.frame = touch_handle_frame,
	.cancel = touch_handle_cancel,
};

// END TOUCH

// BEGIN SEAT

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		uint32_t capabilities) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (seat->wl_pointer == NULL && has_pointer) {
		seat->wl_pointer = wl_seat_get_pointer(wl_seat);
		assert(seat->wl_pointer);
		wl_array_init(&seat->pointer_motion_events);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
		seat->wp_cursor_shape_device =
			wp_cursor_shape_manager_v1_get_pointer(
				yazu->wp_cursor_shape_manager,
				seat->wl_pointer);
		assert(seat->wp_cursor_shape_device);
	} else if (seat->wl_pointer && !has_pointer) {
		destroy_pointer(seat);
	}

	bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
	if (seat->wl_touch == NULL && has_touch) {
		seat->wl_touch = wl_seat_get_touch(wl_seat);
		assert(seat->wl_touch);
		wl_array_init(&seat->wl_touch_events);
		wl_array_init(&seat->touch_points);
		wl_touch_add_listener(seat->wl_touch, &touch_listener, seat);
	} else if (seat->wl_touch && !has_touch) {
		destroy_touch(seat);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
};

// END SEAT

// BEGIN OUTPUT

static void output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct yazu_output *output = data;
	struct yazu *yazu = output->yazu;
	if (output->transform == transform) {
		return;
	}

	output->transform = transform;
	if (yazu->captured_output == output) {
		yazu->transform = transform;
		recompute_dimensions(yazu);

		set_dirty(yazu);
	}
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
};

// END OUTPUT

// BEGIN SHM

static void shm_handle_format(void *data, struct wl_shm *wl_shm,
		uint32_t shm_format) {
	struct yazu *yazu = data;
	enum wl_shm_format *wl_shm_format;
	wl_array_add_last(&yazu->compositor_supported_shm_formats, wl_shm_format);
	*wl_shm_format = shm_format;
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_handle_format,
};

// END SHM

// BEGIN REGISTRY

static void registry_handle_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct yazu *yazu = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		yazu->wl_compositor = wl_registry_bind(wl_registry, name,
			&wl_compositor_interface, 3);
		assert(yazu->wl_compositor);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		yazu->wl_shm = wl_registry_bind(wl_registry, name,
			&wl_shm_interface, 1);
		assert(yazu->wl_shm);
		wl_shm_add_listener(yazu->wl_shm, &shm_listener, yazu);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct yazu_seat *seat = calloc(1, sizeof(struct yazu_seat));
		assert(seat);
		seat->yazu = yazu;
		struct wl_seat *wl_seat = wl_registry_bind(wl_registry, name,
			&wl_seat_interface, 1);
		assert(wl_seat);
		seat->wl_seat = wl_seat;
		wl_seat_add_listener(wl_seat, &seat_listener, seat);
		wl_list_insert(&yazu->seats, &seat->link);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct yazu_output *output = calloc(1, sizeof(struct yazu_output));
		assert(output);
		output->yazu = yazu;
		struct wl_output *wl_output = wl_registry_bind(wl_registry,
			name, &wl_output_interface, 1);
		assert(wl_output);
		output->wl_output = wl_output;
		wl_output_add_listener(wl_output, &output_listener, output);
		wl_list_insert(&yazu->outputs, &output->link);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		yazu->wp_viewporter = wl_registry_bind(wl_registry, name,
			&wp_viewporter_interface, 1);
		assert(yazu->wp_viewporter);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		yazu->wp_cursor_shape_manager = wl_registry_bind(wl_registry,
			name, &wp_cursor_shape_manager_v1_interface, 1);
		assert(yazu->wp_cursor_shape_manager);
	} else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		yazu->wp_single_pixel_buffer_manager = wl_registry_bind(
			wl_registry, name,
			&wp_single_pixel_buffer_manager_v1_interface, 1);
		assert(yazu->wp_single_pixel_buffer_manager);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		yazu->ext_image_copy_capture_manager = wl_registry_bind(
			wl_registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
		assert(yazu->ext_image_copy_capture_manager);
	} else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		yazu->ext_output_image_capture_source_manager = wl_registry_bind(
			wl_registry, name,
			&ext_output_image_capture_source_manager_v1_interface, 1);
		assert(yazu->ext_output_image_capture_source_manager);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		yazu->zwlr_layer_shell = wl_registry_bind(wl_registry, name,
			&zwlr_layer_shell_v1_interface, 3);
		assert(yazu->zwlr_layer_shell);
	}
}

static void registry_handle_global_remove(void *data, struct wl_registry
		*registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

// END REGISTRY

// BEGIN IMAGE COPY FRAME

static void ext_image_copy_capture_frame_handle_transform(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform) {
}

static void ext_image_copy_capture_frame_handle_damage(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, int32_t x, int32_t y,
		int32_t width, int32_t height) {
}

static void ext_image_copy_capture_frame_handle_presentation_time(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t tv_sec_hi,
		uint32_t tv_sec_lo, uint32_t tv_nsec) {
}

static void ext_image_copy_capture_frame_handle_ready(void *data,
		struct ext_image_copy_capture_frame_v1 *frame) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	struct yazu_buffer *buffer = capture->buffer;

	reorder_bytes_to_big_endian_rgbx(buffer->data, buffer->size,
		capture->shm_format);
	capture->frame_ready = true;
	recompute_dimensions(yazu);
}

static void ext_image_copy_capture_frame_handle_failed(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	fprintf(stderr, "failed to copy frame from output\n");
	yazu->failed = true;
	yazu->running = false;
}

static const struct ext_image_copy_capture_frame_v1_listener ext_image_copy_capture_frame_listener = {
	.transform = ext_image_copy_capture_frame_handle_transform,
	.damage = ext_image_copy_capture_frame_handle_damage,
	.presentation_time = ext_image_copy_capture_frame_handle_presentation_time,
	.ready = ext_image_copy_capture_frame_handle_ready,
	.failed = ext_image_copy_capture_frame_handle_failed,
};

// END IMAGE COPY FRAME

// BEGIN IMAGE COPY SESSION

static void ext_image_copy_capture_session_handle_buffer_size(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t width, uint32_t height) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	capture->buffer_width = width;
	capture->buffer_height = height;
	yazu->capture_target_x = capture->buffer_width / 2.0f;
	yazu->capture_target_y = capture->buffer_height / 2.0f;
}

static bool compositor_is_shm_format_supported(struct yazu *yazu, uint32_t shm_format) {
	enum wl_shm_format *compositor_format;
	wl_array_for_each(compositor_format, &yazu->compositor_supported_shm_formats) {
		if (*compositor_format == shm_format) {
			return true;
		}
	}

	return false;
}

static void ext_image_copy_capture_session_handle_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t shm_format) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	if (capture->has_shm_format) {
		return;
	}

	if (compositor_is_shm_format_supported(yazu, shm_format) &&
			client_is_shm_format_supported(shm_format)) {
		capture->shm_format = shm_format;
		capture->has_shm_format = true;
	}
}

static void ext_image_copy_capture_session_handle_dmabuf_device(void *data,
		struct ext_image_copy_capture_session_v1 *session,
		struct wl_array *dev_id_array) {
}

static void ext_image_copy_capture_session_handle_dmabuf_format(void *data,
		struct ext_image_copy_capture_session_v1 *session,
		uint32_t format, struct wl_array *modifiers_array) {
}

static void ext_image_copy_capture_session_handle_done(void *data,
		struct ext_image_copy_capture_session_v1 *session) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	if (capture->capture_started) {
		return;
	}

	capture->capture_started = true;
	if (!capture->has_shm_format) {
		fprintf(stderr, "no supported format found for output frame\n");

		goto error;
	}

	capture->buffer = create_buffer(yazu->wl_shm, capture->buffer_width, capture->buffer_height, capture->shm_format);
	if (capture->buffer == NULL) {
		fprintf(stderr, "failed to create buffer for output frame\n");

		goto error;
	}

	capture->buffer->busy = true;
	capture->ext_image_copy_capture_frame = ext_image_copy_capture_session_v1_create_frame(session);
	assert(capture->ext_image_copy_capture_frame);
	ext_image_copy_capture_frame_v1_add_listener(capture->ext_image_copy_capture_frame,
		&ext_image_copy_capture_frame_listener, capture);
	ext_image_copy_capture_frame_v1_attach_buffer(capture->ext_image_copy_capture_frame,
		capture->buffer->wl_buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(capture->ext_image_copy_capture_frame,
		0, 0, capture->buffer_width, capture->buffer_height);
	ext_image_copy_capture_frame_v1_capture(capture->ext_image_copy_capture_frame);

	return;

error:
	yazu->failed = true;
	yazu->running = false;

	return;
}

static void ext_image_copy_capture_session_handle_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *session) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	if (!capture->capture_started) {
		fprintf(stderr, "capture session closed before frame could be captured\n");
		yazu->failed = true;
		yazu->running = false;

		return;
	}
}

static const struct ext_image_copy_capture_session_v1_listener ext_image_copy_capture_session_listener = {
	.buffer_size = ext_image_copy_capture_session_handle_buffer_size,
	.shm_format = ext_image_copy_capture_session_handle_shm_format,
	.dmabuf_device = ext_image_copy_capture_session_handle_dmabuf_device,
	.dmabuf_format = ext_image_copy_capture_session_handle_dmabuf_format,
	.done = ext_image_copy_capture_session_handle_done,
	.stopped = ext_image_copy_capture_session_handle_stopped,
};

// END IMAGE COPY SESSION

// BEGIN SURFACE

static struct yazu_output * yazu_output_from_wl_output(struct yazu *yazu, struct wl_output *wl_output) {
	struct yazu_output *output;
	wl_list_for_each(output, &yazu->outputs, link) {
		if (output->wl_output == wl_output) {
			return output;
		}
	}

	assert(false);
}

static void surface_handle_enter(void *data, struct wl_surface *wl_surface,
		struct wl_output *wl_output) {
	struct yazu *yazu = data;
	struct yazu_output *output_to_capture = yazu_output_from_wl_output(yazu, wl_output);
	assert(yazu->configured);
	yazu->captured_output = output_to_capture;
	yazu->transform = output_to_capture->transform;
	recompute_dimensions(yazu);
	uint32_t capture_options = 0;
	// TODO: make this configurable
	if (true) {
		capture_options |= EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;
	}
	struct ext_image_capture_source_v1 *output_source = ext_output_image_capture_source_manager_v1_create_source(
		yazu->ext_output_image_capture_source_manager, wl_output);
	assert(output_source);
	yazu->capture.ext_image_copy_capture_session = ext_image_copy_capture_manager_v1_create_session(
		yazu->ext_image_copy_capture_manager, output_source, capture_options);
	assert(yazu->capture.ext_image_copy_capture_session);
	ext_image_copy_capture_session_v1_add_listener(yazu->capture.ext_image_copy_capture_session,
		&ext_image_copy_capture_session_listener, &yazu->capture);
	ext_image_capture_source_v1_destroy(output_source);
}

static void surface_handle_leave(void *data, struct wl_surface *wl_surface,
		struct wl_output *wl_output) {
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_handle_enter,
	.leave = surface_handle_leave,
};

// END SURFACE

// BEGIN LAYER SURFACE

static void layer_surface_handle_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct yazu *yazu = data;
	yazu->configured = true;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	bool dimensions_changed = yazu->width != width || yazu->height != height;
	if (dimensions_changed) {
		yazu->width = width;
		yazu->height = height;
		wp_viewport_set_destination(yazu->wp_viewport, yazu->width, yazu->height);
		recompute_dimensions(yazu);

		set_dirty(yazu);
	}
	if (!dimensions_changed || yazu->gl_initialized) {
		return;
	}

	// map a completely transparent surface initially so that we can
	// determine the output to capture from the surface's enter event
	struct wl_buffer *wl_buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		yazu->wp_single_pixel_buffer_manager, 0, 0, 0, 0);

	setup_viewport_source(yazu, 0, 0, 1, 1);

	wl_surface_attach(yazu->wl_surface, wl_buffer, 0, 0);
	wl_surface_damage(yazu->wl_surface, 0, 0, yazu->width, yazu->height);
	wl_surface_commit(yazu->wl_surface);

	wl_buffer_destroy(wl_buffer);
}

static void layer_surface_handle_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct yazu *yazu = data;
	yazu->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
};

// END LAYER SURFACE

// BEGIN SURFACE FRAME

static void surface_frame_handle_done(void *data,
		struct wl_callback *wl_callback, uint32_t time) {
	struct yazu *yazu = data;
	wl_callback_destroy(wl_callback);
	yazu->surface_frame_callback = NULL;

	if (yazu->dirty) {
		process_animations(yazu, time);
		setup_surface_frame_callback(yazu);
		send_frame(yazu);
	}
}

static const struct wl_callback_listener surface_frame_listener = {
	.done = surface_frame_handle_done,
};

// END SURFACE FRAME

static void setup_surface_frame_callback(struct yazu *yazu) {
	yazu->surface_frame_callback = wl_surface_frame(yazu->wl_surface);
	assert(yazu->surface_frame_callback);
	wl_callback_add_listener(yazu->surface_frame_callback,
		&surface_frame_listener, yazu);
}


static void setup_viewport_source(struct yazu *yazu, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	wl_fixed_t viewport_x, viewport_y, viewport_width, viewport_height;

	viewport_x = wl_fixed_from_int(x);
	viewport_y = wl_fixed_from_int(y);
	viewport_width = wl_fixed_from_int(width);
	viewport_height = wl_fixed_from_int(height);
	wp_viewport_set_source(yazu->wp_viewport, viewport_x, viewport_y, viewport_width, viewport_height);
}

static void set_dirty(struct yazu *yazu) {
	yazu->dirty = true;
	if (yazu->surface_frame_callback || !yazu->gl_initialized) {
		return;
	}

	setup_surface_frame_callback(yazu);
	wl_surface_commit(yazu->wl_surface);
}

static void render(struct yazu *yazu) {
	static size_t vertices_stride = 4;
	static GLfloat vertices[] = {
		// v_pos   t_pos
		   -1, -1, 0, 1, // bottom left
		    1, -1, 1, 1, // bottom right
		    1,  1, 1, 0, // top right
		   -1,  1, 0, 0, // top left
	};
	static GLushort indices[] = {
		0, 1, 3, // first triangle
		1, 2, 3, // second triangle
	};
	struct yazu_capture *capture = &yazu->capture;
	double capture_sample_sx, capture_sample_sy, capture_sample_ex,
		capture_sample_ey;

	capture_sample_sx = buffer_x_to_capture_x(yazu, 0);
	capture_sample_sy = buffer_y_to_capture_y(yazu, 0);
	capture_sample_ex = buffer_x_to_capture_x(yazu, yazu->buffer_width);
	capture_sample_ey = buffer_y_to_capture_y(yazu, yazu->buffer_height);

	capture_sample_sx /= capture->buffer_width;
	capture_sample_sy /= capture->buffer_height;
	capture_sample_ex /= capture->buffer_width;
	capture_sample_ey /= capture->buffer_height;

	vertices[2 + 0 * vertices_stride] = capture_sample_sx;
	vertices[3 + 0 * vertices_stride] = capture_sample_ey;

	vertices[2 + 1 * vertices_stride] = capture_sample_ex;
	vertices[3 + 1 * vertices_stride] = capture_sample_ey;

	vertices[2 + 2 * vertices_stride] = capture_sample_ex;
	vertices[3 + 2 * vertices_stride] = capture_sample_sy;

	vertices[2 + 3 * vertices_stride] = capture_sample_sx;
	vertices[3 + 3 * vertices_stride] = capture_sample_sy;

	glVertexAttribPointer(yazu->gl.vertex_position, 2, GL_FLOAT, GL_FALSE,
		vertices_stride * sizeof(float), vertices);
	glVertexAttribPointer(yazu->gl.texture_position, 2, GL_FLOAT, GL_FALSE,
		vertices_stride * sizeof(float), &vertices[2]);
	glEnableVertexAttribArray(yazu->gl.vertex_position);
	glEnableVertexAttribArray(yazu->gl.texture_position);

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawElements(GL_TRIANGLES, sizeof(indices) / sizeof(GLushort),
		GL_UNSIGNED_SHORT, indices);
}

static void send_frame(struct yazu *yazu) {
	// TODO: Is it worth only calling these functions if necessary?
	wl_surface_set_buffer_transform(yazu->wl_surface, yazu->transform);
	setup_viewport_source(yazu, 0, 0, yazu->transformed_buffer_width,
		yazu->transformed_buffer_height);

	render(yazu);

	eglSwapBuffers(yazu->egl.display, yazu->egl_surface);

	yazu->dirty = yazu->sliding || yazu->zooming;
}

static double real_zoom_scale(struct yazu *yazu) {
	return pow(2, (yazu->zoom_scale - 1));
}

static double buffer_x_to_capture_x(struct yazu *yazu, double buffer_x) {
	return yazu->capture_target_x + (buffer_x - yazu->half_buffer_width) / real_zoom_scale(yazu);
}

static double buffer_y_to_capture_y(struct yazu *yazu, double buffer_y) {
	return yazu->capture_target_y + (buffer_y - yazu->half_buffer_height) / real_zoom_scale(yazu);
}

static bool clamp_capture_target_x(struct yazu *yazu) {
	bool did_clamp = false;
	double buffer_left_capture_x = buffer_x_to_capture_x(yazu, 0);
	double buffer_right_capture_x = buffer_x_to_capture_x(yazu, yazu->buffer_width);
	if (buffer_left_capture_x < 0) {
		yazu->capture_target_x -= buffer_left_capture_x;
		did_clamp = true;
	} else if (buffer_right_capture_x > yazu->capture.buffer_width) {
		yazu->capture_target_x -= (buffer_right_capture_x - yazu->capture.buffer_width);
		did_clamp = true;
	}

	return did_clamp;
}
static bool clamp_capture_target_y(struct yazu *yazu) {
	bool did_clamp = false;
	double buffer_top_capture_y = buffer_y_to_capture_y(yazu, 0);
	double buffer_bottom_capture_y = buffer_y_to_capture_y(yazu, yazu->buffer_height);
	if (buffer_top_capture_y < 0) {
		yazu->capture_target_y -= buffer_top_capture_y;
		did_clamp = true;
	} else if (buffer_bottom_capture_y > yazu->capture.buffer_height) {
		yazu->capture_target_y -= (buffer_bottom_capture_y - yazu->capture.buffer_height);
		did_clamp = true;
	}

	return did_clamp;
}

static bool clamp_capture_target(struct yazu *yazu) {
	bool clamped_x, clamped_y;
	clamped_x = clamp_capture_target_x(yazu);
	clamped_y = clamp_capture_target_y(yazu);

	return clamped_x || clamped_y;
}

static void drag_capture(struct yazu *yazu, double old_buffer_x, double old_buffer_y,
		double buffer_x, double buffer_y) {
	double old_buffer_x_capture_space, old_buffer_y_capture_space;
	double buffer_x_capture_space, buffer_y_capture_space;
	double buffer_drag_diff_x, buffer_drag_diff_y;

	old_buffer_x_capture_space = buffer_x_to_capture_x(yazu, old_buffer_x);
	old_buffer_y_capture_space = buffer_y_to_capture_y(yazu, old_buffer_y);
	buffer_x_capture_space = buffer_x_to_capture_x(yazu, buffer_x);
	buffer_y_capture_space = buffer_y_to_capture_y(yazu, buffer_y);
	buffer_drag_diff_x = buffer_x_capture_space - old_buffer_x_capture_space;
	buffer_drag_diff_y = buffer_y_capture_space - old_buffer_y_capture_space;
	yazu->capture_target_x -= buffer_drag_diff_x;
	yazu->capture_target_y -= buffer_drag_diff_y;
	clamp_capture_target(yazu);

	set_dirty(yazu);
}

static void get_transformed_buffer_dimensions(
		enum wl_output_transform transform, uint32_t old_width,
		uint32_t old_height, uint32_t *new_width, uint32_t *new_height) {
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*new_width = old_width;
		*new_height = old_height;

		return;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*new_width = old_height;
		*new_height = old_width;

		return;
	}
	assert(false);
}

static void recompute_dimensions(struct yazu *yazu) {
	struct yazu_capture *capture = &yazu->capture;
	if (capture->frame_ready) {
		yazu->buffer_width = capture->buffer_width;
		yazu->buffer_height = capture->buffer_height;
	} else {
		yazu->buffer_width = 1;
		yazu->buffer_height = 1;
	}
	yazu->half_buffer_width = yazu->buffer_width / 2.0f;
	yazu->half_buffer_height = yazu->buffer_height / 2.0f;
	get_transformed_buffer_dimensions(yazu->transform, yazu->buffer_width,
		yazu->buffer_height, &yazu->transformed_buffer_width,
		&yazu->transformed_buffer_height);
	yazu->buffer_scale_x =
		((double) yazu->transformed_buffer_width) / yazu->width;
	yazu->buffer_scale_y =
		((double) yazu->transformed_buffer_height) / yazu->height;
	yazu->estimated_output_scale = sqrt(
		yazu->buffer_scale_x * yazu->buffer_scale_y);
}

static void trim_old_motion_events(struct wl_array *motion_events,
		uint32_t time) {
	struct yazu_input_motion_event *motion_event;
	uint32_t elapsed_time;
	for (size_t i = 0; i < motion_events->size; i += sizeof(struct yazu_input_motion_event)) {
		motion_event = motion_events->data + i;
		elapsed_time = time - motion_event->time;
		if (elapsed_time > SAMPLE_IS_OLD_THRESHOLD) {
			motion_events->size = i;

			break;
		}
	}
}

static void add_motion_event(struct wl_array *motion_events, double x,
		double y, uint32_t time) {
	struct yazu_input_motion_event *motion_event;
	wl_array_add_first(motion_events, motion_event);
	motion_event->x = x;
	motion_event->y = y;
	motion_event->time = time;

	trim_old_motion_events(motion_events, time);
}

static void handle_drag_release(struct yazu* yazu,
		struct wl_array *motion_events, uint32_t time) {
	trim_old_motion_events(motion_events, time);

	size_t num_events = motion_events->size / sizeof(struct yazu_input_motion_event);

	struct yazu_input_motion_event *first, *last;
	last = motion_events->data;
	first = last + (num_events - 1);

	uint32_t dt = last->time - first->time;
	if (dt <= 0) {
		return;
	}

	double dx, dy;
	dx = last->x - first->x;
	dy = last->y - first->y;

	if (dx == 0 || dy == 0) {
		return;
	}

	yazu->sliding = true;
	yazu->slide_last_tick_time = time;

	// buffer space velocity in pixels per millisecond
	double vx, vy;
	vx = dx / dt;
	vy = dy / dt;

	// capture space capture target velocity in pixels per millisecond
	yazu->slide_x_velocity = -vx / real_zoom_scale(yazu);
	yazu->slide_y_velocity = -vy / real_zoom_scale(yazu);
	double slide_velocity = sqrt(
		squared_distance(
			yazu->slide_x_velocity,
			yazu->slide_y_velocity));
	double acceleration_magnitude =
		(-0.01 * yazu->estimated_output_scale) / real_zoom_scale(yazu);
	yazu->slide_x_acceleration =
		acceleration_magnitude * (yazu->slide_x_velocity / slide_velocity);
	yazu->slide_y_acceleration =
		acceleration_magnitude * (yazu->slide_y_velocity / slide_velocity);

	set_dirty(yazu);
}

static double squared_distance(double dx, double dy) {
	return dx * dx + dy * dy;
}

static void process_zooming(struct yazu *yazu, uint32_t time) {
	if (!yazu->zooming) {
		return;
	}

	uint32_t dt;
	struct yazu_seat *seat;
	double capture_x_at_cursor, capture_y_at_cursor;
	double new_capture_x_at_cursor, new_capture_y_at_cursor;
	double time_scale;
	double last_tick_scaled_time;
	double scaled_time;
	double stop_time;
	double offset_time;
	double coefficient;

	dt = time - yazu->zoom_last_tick_time;
	assert(dt >= 0);
	seat = yazu->zoom_seat;
	capture_x_at_cursor = buffer_x_to_capture_x(yazu, seat->cursor_x);
	capture_y_at_cursor = buffer_y_to_capture_y(yazu, seat->cursor_y);
	time_scale = 30;
	last_tick_scaled_time = yazu->zoom_last_tick_time / time_scale;
	scaled_time = time / time_scale;

	/* y = x^2 */
	/* zoom_percent = t^2 */
	/* zoom_percent = t^2 + zoom_target_percent */
	/* zoom_percent = (zoom_last_tick_time - o)^2 + zoom_target_percent */
	/* zoom_percent - zoom_target_percent = (zoom_last_tick_time - o)^2 */
	/* -sqrt(zoom_percent - zoom_target_percent) = zoom_last_tick_time - o */
	/* -sqrt(zoom_percent - zoom_target_percent) - zoom_last_tick_time = -o */
	/* o = sqrt(zoom_percent - zoom_target_percent) + zoom_last_tick_time */

	stop_time = sqrt(fabs(yazu->zoom_percent - yazu->zoom_target_percent)) + last_tick_scaled_time;
	offset_time = scaled_time - stop_time;
	if (offset_time >= 0) {
		yazu->zoom_percent = yazu->zoom_target_percent;
	} else {
		coefficient = (yazu->zoom_target_percent - yazu->zoom_percent > 0) ? -1 : 1;
		yazu->zoom_percent = coefficient * (offset_time * offset_time) + yazu->zoom_target_percent;
	}
	yazu->zoom_scale = yazu->zoom_percent / 100;
	yazu->zoom_last_tick_time = time;

	new_capture_x_at_cursor = buffer_x_to_capture_x(yazu, seat->cursor_x);
	new_capture_y_at_cursor = buffer_y_to_capture_y(yazu, seat->cursor_y);
	yazu->capture_target_x -= (new_capture_x_at_cursor - capture_x_at_cursor);
	yazu->capture_target_y -= (new_capture_y_at_cursor - capture_y_at_cursor);
	clamp_capture_target(yazu);

	if (yazu->zoom_percent == yazu->zoom_target_percent) {
		yazu->zooming = false;
	}
}

static void process_sliding(struct yazu *yazu, uint32_t time) {
	if (!yazu->sliding) {
		return;
	}

	uint32_t dt;
	double dx, dy;
	double ax, ay;
	double initial_vx, initial_vy;
	double current_vx, current_vy;
	double stop_dt_x, stop_dt_y;
	bool clamped_x, clamped_y;
	bool sliding_on_x, sliding_on_y;

	dt = time - yazu->slide_last_tick_time;
	assert(dt >= 0);
	ax = yazu->slide_x_acceleration;
	ay = yazu->slide_y_acceleration;
	initial_vx = yazu->slide_x_velocity;
	initial_vy = yazu->slide_y_velocity;
	current_vx = initial_vx + ax * dt;
	current_vy = initial_vy + ay * dt;
	if (ax != 0) {
		stop_dt_x = -initial_vx / ax;
	} else {
		stop_dt_x = UINT32_MAX;
	}
	if (ay != 0) {
		stop_dt_y = -initial_vy / ay;
	} else {
		stop_dt_y = UINT32_MAX;
	}
	if (dt >= stop_dt_x) {
		current_vx = 0;
		yazu->slide_x_acceleration = 0;
	}
	if (dt >= stop_dt_y) {
		current_vy = 0;
		yazu->slide_y_acceleration = 0;
	}
	if (ax != 0) {
		dx = (current_vx * current_vx - initial_vx * initial_vx) / (2 * ax);
	} else {
		dx = initial_vx * dt;
	}
	if (ay != 0) {
		dy = (current_vy * current_vy - initial_vy * initial_vy) / (2 * ay);
	} else {
		dy = initial_vy * dt;
	}

	yazu->capture_target_x += dx;
	yazu->capture_target_y += dy;
	yazu->slide_x_velocity = current_vx;
	yazu->slide_y_velocity = current_vy;
	yazu->slide_last_tick_time = time;

	clamped_x = clamp_capture_target_x(yazu);
	clamped_y = clamp_capture_target_y(yazu);
	if (clamped_x) {
		yazu->slide_x_velocity = 0;
		yazu->slide_x_acceleration = 0;
	}
	if (clamped_y) {
		yazu->slide_y_velocity = 0;
		yazu->slide_y_acceleration = 0;
	}
	sliding_on_x = yazu->slide_x_velocity != 0 || yazu->slide_x_acceleration != 0;
	sliding_on_y = yazu->slide_y_velocity != 0 || yazu->slide_y_acceleration != 0;
	if (!sliding_on_x && !sliding_on_y) {
		yazu->sliding = false;
	}
}

static void process_animations(struct yazu *yazu, uint32_t time) {
	process_zooming(yazu, time);
	process_sliding(yazu, time);
}

static void destroy_pointer(struct yazu_seat *seat) {
	if (seat->wp_cursor_shape_device) {
		wp_cursor_shape_device_v1_destroy(seat->wp_cursor_shape_device);
	}
	if (seat->wl_pointer) {
		wl_pointer_destroy(seat->wl_pointer);
	}
	wl_array_release(&seat->pointer_motion_events);
}

static void destroy_touch(struct yazu_seat *seat) {
	if (seat->wl_touch) {
		wl_touch_destroy(seat->wl_touch);
	}

	struct wl_array *touch_points = &seat->touch_points;
	struct yazu_touch_point *touch_point;
	wl_array_for_each(touch_point, touch_points) {
		wl_array_release(&touch_point->motion_events);
	}
	wl_array_release(touch_points);

	wl_array_release(&seat->wl_touch_events);
}

static void destroy_capture(struct yazu_capture *capture) {
	if (capture->ext_image_copy_capture_frame) {
		ext_image_copy_capture_frame_v1_destroy(capture->ext_image_copy_capture_frame);
	}
	if (capture->ext_image_copy_capture_session) {
		ext_image_copy_capture_session_v1_destroy(capture->ext_image_copy_capture_session);
	}
	destroy_buffer(capture->buffer);
}

static void initialize_egl(struct yazu *yazu) {
	EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_NONE
	};
	EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLBoolean result;
	EGLConfig *matching_configs;
	EGLint num_matching_configs;

	yazu->egl.display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, yazu->display, NULL);
	assert(yazu->egl.display != EGL_NO_DISPLAY);
	result = eglInitialize(yazu->egl.display, NULL, NULL);
	assert(result == EGL_TRUE);
	result = eglBindAPI(EGL_OPENGL_ES_API);
	assert(result == EGL_TRUE);

	result = eglChooseConfig(yazu->egl.display, config_attributes, NULL, 0, &num_matching_configs);
	assert(result == EGL_TRUE && num_matching_configs);
	matching_configs = calloc(num_matching_configs, sizeof(EGLConfig));
	assert(matching_configs);
	result = eglChooseConfig(yazu->egl.display, config_attributes, matching_configs, num_matching_configs, &num_matching_configs);
	assert(result == EGL_TRUE && num_matching_configs);
	/* for (EGLint i = 0; i < num_matching_configs; i++) { */
	/* 	EGLConfig config = matching_configs[i]; */
	/* 	EGLint buffer_bpp, red_size, green_size, blue_size, alpha_size; */
	/* 	eglGetConfigAttrib(yazu->egl.display, config, EGL_BUFFER_SIZE, &buffer_bpp); */
	/* 	eglGetConfigAttrib(yazu->egl.display, config, EGL_RED_SIZE, &red_size); */
	/* 	eglGetConfigAttrib(yazu->egl.display, config, EGL_GREEN_SIZE, &green_size); */
	/* 	eglGetConfigAttrib(yazu->egl.display, config, EGL_BLUE_SIZE, &blue_size); */
	/* 	eglGetConfigAttrib(yazu->egl.display, config, EGL_ALPHA_SIZE, &alpha_size); */
	/* 	printf("buffer_bpp: %d, red_size: %d, green_size: %d, blue_size: %d, alpha_size: %d\n", */
	/* 		buffer_bpp, red_size, green_size, blue_size, alpha_size); */
	/* } */
	yazu->egl.config = matching_configs[0];
	free(matching_configs);

	yazu->egl.context = eglCreateContext(yazu->egl.display, yazu->egl.config, EGL_NO_CONTEXT, context_attributes);
	assert(yazu->egl.context != EGL_NO_CONTEXT);
}

static void terminate_egl(struct yazu *yazu) {
	EGLint result;
	result = eglTerminate(yazu->egl.display);
	assert(result == EGL_TRUE);
	result = eglReleaseThread();
	assert(result == EGL_TRUE);
}

static void initialize_egl_surface(struct yazu *yazu) {
	EGLAttrib attributes[1] = { EGL_NONE };
	EGLBoolean result;
	yazu->wl_egl_window = wl_egl_window_create(yazu->wl_surface,
		yazu->buffer_width, yazu->buffer_height);
	assert(yazu->wl_egl_window);
	yazu->egl_surface = eglCreatePlatformWindowSurface(yazu->egl.display,
		yazu->egl.config, yazu->wl_egl_window, attributes);
	assert(yazu->egl_surface != EGL_NO_SURFACE);
	result = eglMakeCurrent(yazu->egl.display, yazu->egl_surface,
		yazu->egl_surface, yazu->egl.context);
	assert(result == EGL_TRUE);
	eglSwapInterval(yazu->egl.display, 0);
}

static void terminate_egl_surface(struct yazu *yazu) {
	EGLint result;
	result = eglMakeCurrent(yazu->egl.display,
		EGL_NO_SURFACE, EGL_NO_SURFACE,
		EGL_NO_CONTEXT);
	assert(result == EGL_TRUE);
	result = eglDestroySurface(yazu->egl.display, yazu->egl_surface);
	assert(result == EGL_TRUE);
	wl_egl_window_destroy(yazu->wl_egl_window);
}

static GLuint build_shader(const char *source, GLenum shader_type) {
	GLuint shader;
	GLint status;

	shader = glCreateShader(shader_type);
	assert(shader);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		const size_t log_max_length = 1024;
		char log[log_max_length];
		GLsizei log_length;
		glGetShaderInfoLog(shader, log_max_length, &log_length, log);
		fprintf(stderr, "error while compiling %s shader: %.*s\n",
			shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
			log_length, log);

		return 0;
	}

	return shader;
}

static bool initialize_gl(struct yazu *yazu) {
	struct yazu_capture *capture = &yazu->capture;
	GLuint vertex_shader, fragment_shader, program, capture_texture;
	GLint status;

	vertex_shader = build_shader(vertex_shader_string, GL_VERTEX_SHADER);
	fragment_shader = build_shader(fragment_shader_string, GL_FRAGMENT_SHADER);
	if (vertex_shader == 0 || fragment_shader == 0) {
		return false;
	}

	program = glCreateProgram();
	if (program == 0) {
		return false;
	}

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);

	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		const size_t log_max_length = 1024;
		char log[log_max_length];
		GLsizei log_length;
		glGetProgramInfoLog(program, log_max_length, &log_length, log);
		fprintf(stderr, "error while linking program: %.*s\n",
			log_length, log);

		return false;
	}

	glUseProgram(program);

	yazu->gl.vertex_position = 0;
	yazu->gl.texture_position = 1;
	glBindAttribLocation(program, yazu->gl.vertex_position, "vertex_position");
	glBindAttribLocation(program, yazu->gl.texture_position, "texture_position");

	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &capture_texture);
	glBindTexture(GL_TEXTURE_2D, capture_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, capture->buffer_width,
		capture->buffer_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		capture->buffer->data);
	glUniform1i(glGetUniformLocation(program, "capture_texture"), GL_TEXTURE0);

	glViewport(0, 0, yazu->buffer_width, yazu->buffer_height);

	return true;
}

int main(int argc, char **argv) {
	bool ret_code = EXIT_FAILURE;
	struct yazu yazu = {
		.running = true,
		.zoom_percent = 100,
		.zoom_target_percent = 100,
		.zoom_scale = 1,
	};
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to Wayland display\n");

		return EXIT_FAILURE;
	}

	yazu.display = display;
	wl_array_init(&yazu.compositor_supported_shm_formats);
	wl_list_init(&yazu.seats);
	wl_list_init(&yazu.outputs);
	struct wl_registry *wl_registry = wl_display_get_registry(display);
	assert(wl_registry);
	wl_registry_add_listener(wl_registry, &registry_listener, &yazu);

	// roundtrip for registry
	wl_display_roundtrip(display);

	bool unsupported_compositor = false;
#define verify_global_object_exists(object_name) \
	if (yazu.object_name == NULL) { \
		fprintf(stderr, "compositor doesn't support " #object_name "\n"); \
		unsupported_compositor = true; \
	}
	verify_global_object_exists(wl_compositor);
	verify_global_object_exists(wl_shm);
	verify_global_object_exists(wp_viewporter);
	verify_global_object_exists(wp_cursor_shape_manager);
	verify_global_object_exists(wp_single_pixel_buffer_manager);
	verify_global_object_exists(ext_image_copy_capture_manager);
	verify_global_object_exists(ext_output_image_capture_source_manager);
	verify_global_object_exists(zwlr_layer_shell);
	if (unsupported_compositor) {
		goto cleanup_bindings;
	}
#undef verify_global_object_exists

	// roundtrip for supported shm formats
	wl_display_roundtrip(display);

	initialize_egl(&yazu);

	yazu.wl_surface = wl_compositor_create_surface(yazu.wl_compositor);
	assert(yazu.wl_surface);
	yazu.wp_viewport = wp_viewporter_get_viewport(yazu.wp_viewporter, yazu.wl_surface);
	assert(yazu.wp_viewport);
	wl_surface_add_listener(yazu.wl_surface, &surface_listener, &yazu);
	yazu.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		yazu.zwlr_layer_shell, yazu.wl_surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "zoom");
	assert(yazu.layer_surface);
	zwlr_layer_surface_v1_set_anchor(yazu.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
	zwlr_layer_surface_v1_set_keyboard_interactivity(yazu.layer_surface,
		1);
	zwlr_layer_surface_v1_set_exclusive_zone(yazu.layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(yazu.layer_surface,
		&layer_surface_listener, &yazu);
	wl_surface_commit(yazu.wl_surface);

	int num_dispatched = 0;
	while (yazu.running &&
			(num_dispatched = wl_display_dispatch(display)) != -1 &&
			!yazu.capture.frame_ready) {
	}
	if (!yazu.running || num_dispatched == -1) {
		goto cleanup;
	}

	assert(yazu.capture.frame_ready);

	initialize_egl_surface(&yazu);
	if (!initialize_gl(&yazu)) {
		yazu.failed = true;

		goto terminate_egl_surface;
	}

	yazu.gl_initialized = true;

	set_dirty(&yazu);

	while (yazu.running &&
			(num_dispatched = wl_display_dispatch(display)) != -1) {
	}

terminate_egl_surface:
	terminate_egl_surface(&yazu);

cleanup:
	ret_code = yazu.failed || num_dispatched == -1;

	destroy_capture(&yazu.capture);
	zwlr_layer_surface_v1_destroy(yazu.layer_surface);
	wl_surface_destroy(yazu.wl_surface);
	wp_viewport_destroy(yazu.wp_viewport);

	if (yazu.surface_frame_callback) {
		wl_callback_destroy(yazu.surface_frame_callback);
	}

	terminate_egl(&yazu);

cleanup_bindings:
#define destroy_global_object_if_exists(object_name, version_id) \
	if (yazu.object_name) { \
		object_name ## version_id ## _destroy(yazu.object_name); \
	}
	destroy_global_object_if_exists(zwlr_layer_shell, _v1);
	destroy_global_object_if_exists(ext_output_image_capture_source_manager, _v1);
	destroy_global_object_if_exists(ext_image_copy_capture_manager, _v1);
	wl_array_release(&yazu.compositor_supported_shm_formats);
	struct yazu_seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &yazu.seats, link) {
		wl_list_remove(&seat->link);
		destroy_pointer(seat);
		destroy_touch(seat);
		wl_seat_destroy(seat->wl_seat);
		free(seat);
	}
	struct yazu_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &yazu.outputs, link) {
		wl_list_remove(&output->link);
		wl_output_destroy(output->wl_output);
		free(output);
	}
	destroy_global_object_if_exists(wp_viewporter,);
	destroy_global_object_if_exists(wp_cursor_shape_manager, _v1);
	destroy_global_object_if_exists(wp_single_pixel_buffer_manager, _v1);
	destroy_global_object_if_exists(wl_shm,);
	destroy_global_object_if_exists(wl_compositor,);
#undef destroy_global_object_if_exists
	wl_registry_destroy(wl_registry);

	// ensure all queued requests have been received by server
	wl_display_roundtrip(display);

	wl_display_disconnect(display);

	return ret_code;
}
