#include "yazu.h"
#include "buffer.h"

// milliseconds
#define SAMPLE_IS_OLD_THRESHOLD 50

static void setup_surface_frame_callback(struct yazu *yazu);

static void set_dirty(struct yazu *yazu);

static bool send_frame(struct yazu *yazu);

static double real_zoom_scale(struct yazu *yazu);

static double buffer_x_to_capture_x(struct yazu *yazu, double buffer_x);

static double buffer_y_to_capture_y(struct yazu *yazu, double buffer_y);

static bool clamp_capture_target(struct yazu *yazu);

static bool clamp_capture_target_x(struct yazu *yazu);

static bool clamp_capture_target_y(struct yazu *yazu);

static void recompute_dimensions(struct yazu *yazu);

static void trim_old_mouse_samples(struct yazu_seat *seat, uint32_t time);

static double squared_distance(double dx, double dy);

static void process_animations(struct yazu *yazu, uint32_t time);

static bool approximately_equals(double a, double b);

static void destroy_pointer(struct yazu_seat *yazu_seat);

// BEGIN POINTER

static void pointer_set_shape(struct yazu *yazu, struct yazu_seat *seat,
		struct wl_pointer *wl_pointer, uint32_t serial) {
	enum wp_cursor_shape_device_v1_shape shape =
		yazu->dragging ?
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING :
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
	wp_cursor_shape_device_v1_set_shape(seat->wp_cursor_shape_device,
		serial, shape);
}

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	assert(!seat->pointer_on_surface);
	seat->pointer_on_surface = true;
	pointer_set_shape(yazu, seat, wl_pointer, serial);
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct yazu_seat *seat = data;
	assert(seat->pointer_on_surface);
	seat->pointer_on_surface = false;
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	assert(seat->pointer_on_surface);
	seat->cursor_x = wl_fixed_to_double(surface_x) * yazu->scale_x;
	seat->cursor_y = wl_fixed_to_double(surface_y) * yazu->scale_y;
	if (seat->dragging) {
		double cursor_x_capture_space = buffer_x_to_capture_x(yazu, seat->cursor_x);
		double cursor_y_capture_space = buffer_y_to_capture_y(yazu, seat->cursor_y);
		double cursor_grab_diff_x = seat->capture_grab_x - cursor_x_capture_space;
		double cursor_grab_diff_y = seat->capture_grab_y - cursor_y_capture_space;
		yazu->capture_target_x += cursor_grab_diff_x;
		yazu->capture_target_y += cursor_grab_diff_y;
		clamp_capture_target(yazu);

		set_dirty(yazu);
	}

	struct wl_array *motion_events = &seat->motion_events;
	size_t old_motion_events_size = motion_events->size;
	wl_array_add(motion_events, sizeof(struct yazu_mouse_sample));
	struct yazu_mouse_sample *first = motion_events->data;
	memmove(first + 1, first, old_motion_events_size);
	first->x = seat->cursor_x;
	first->y = seat->cursor_y;
	first->time = time;

	trim_old_mouse_samples(seat, time);
}

static void handle_drag_release(struct yazu* yazu, struct yazu_seat* seat, uint32_t time) {
	trim_old_mouse_samples(seat, time);

	struct wl_array *motion_events_array = &seat->motion_events;
	size_t num_events = motion_events_array->size / sizeof(struct yazu_mouse_sample);
	struct yazu_mouse_sample *motion_events = motion_events_array->data;

	struct yazu_mouse_sample *first, *last;
	first = motion_events + (num_events - 1);
	last = motion_events;

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

	// buffer space pointer velocity in pixels per millisecond
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
	double acceleration_magnitude = -0.01 / real_zoom_scale(yazu);
	yazu->slide_x_acceleration =
		acceleration_magnitude * (yazu->slide_x_velocity / slide_velocity);
	yazu->slide_y_acceleration =
		acceleration_magnitude * (yazu->slide_y_velocity / slide_velocity);

	set_dirty(yazu);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button,
		uint32_t button_state) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	assert(seat->pointer_on_surface);
	seat->button_state = button_state;
	seat->last_button = button;
	switch (button) {
	case BTN_LEFT:
		bool is_pressed = button_state == WL_POINTER_BUTTON_STATE_PRESSED;
		if (!yazu->dragging && is_pressed) {
			yazu->sliding = false;
			yazu->dragging = true;
			seat->dragging = true;
			seat->capture_grab_x = buffer_x_to_capture_x(yazu, seat->cursor_x);
			seat->capture_grab_y = buffer_y_to_capture_y(yazu, seat->cursor_y);

			pointer_set_shape(yazu, seat, wl_pointer, serial);
		} else if (seat->dragging && !is_pressed) {
			yazu->dragging = false;
			seat->dragging = false;

			handle_drag_release(yazu, seat, time);
			pointer_set_shape(yazu, seat, wl_pointer, serial);
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

// BEGIN SEAT

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		uint32_t capabilities) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;
	bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (seat->wl_pointer == NULL && has_pointer) {
		seat->wl_pointer = wl_seat_get_pointer(wl_seat);
		assert(seat->wl_pointer);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
		seat->wp_cursor_shape_device =
			wp_cursor_shape_manager_v1_get_pointer(
				yazu->wp_cursor_shape_manager,
				seat->wl_pointer);
		assert(seat->wp_cursor_shape_device);
	} else if (seat->wl_pointer && !has_pointer) {
		destroy_pointer(seat);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
};

// END SEAT

// BEGIN SHM

static void shm_handle_format(void *data, struct wl_shm *wl_shm,
		uint32_t format) {
	struct yazu *yazu = data;
	struct wl_array *shm_formats_array = &yazu->compositor_supported_shm_formats;
	wl_array_add(shm_formats_array, sizeof(enum wl_shm_format));
	size_t num_formats = shm_formats_array->size / sizeof(enum wl_shm_format);
	enum wl_shm_format *last =
		((enum wl_shm_format *) shm_formats_array->data) +
		(num_formats - 1);
	*last = format;
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
		wl_array_init(&seat->motion_events);
		wl_seat_add_listener(wl_seat, &seat_listener, seat);
		wl_list_insert(&yazu->seats, &seat->link);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct yazu_output *output = calloc(1, sizeof(struct yazu_output));
		assert(output);
		struct wl_output *wl_output = wl_registry_bind(wl_registry,
			name, &wl_output_interface, 1);
		assert(wl_output);
		output->wl_output = wl_output;
		wl_list_insert(&yazu->outputs, &output->link);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		yazu->wp_viewporter = wl_registry_bind(wl_registry, name,
			&wp_viewporter_interface, 1);
		assert(yazu->wp_viewporter);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		yazu->wp_cursor_shape_manager = wl_registry_bind(wl_registry,
			name, &wp_cursor_shape_manager_v1_interface, 1);
		assert(yazu->wp_cursor_shape_manager);
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
	struct yazu_capture *capture = data;
	capture->transform = transform;
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
	capture->frame_ready = true;
	recompute_dimensions(yazu);

	set_dirty(yazu);
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

static void surface_handle_enter(void *data, struct wl_surface *wl_surface,
		struct wl_output *wl_output) {
	struct yazu *yazu = data;
	assert(yazu->configured);
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

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void surface_handle_preferred_buffer_scale(void *data, struct wl_surface *wl_surface, int32_t scale) {
}

static void surface_handle_preferred_buffer_transform(void *data, struct wl_surface *wl_surface, uint32_t transform) {
}
#endif

static const struct wl_surface_listener surface_listener = {
	.enter = surface_handle_enter,
	.leave = surface_handle_leave,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
	.preferred_buffer_scale = surface_handle_preferred_buffer_scale,
	.preferred_buffer_transform = surface_handle_preferred_buffer_transform,
#endif
};

// END SURFACE

// BEGIN LAYER SURFACE

static void layer_surface_handle_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct yazu *yazu = data;
	bool is_initial_configure = !yazu->configured;
	bool dimensions_changed = yazu->width != width || yazu->height != height;
	yazu->configured = true;
	if (dimensions_changed) {
		yazu->width = width;
		yazu->height = height;
		recompute_dimensions(yazu);
	}

	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	if (!is_initial_configure) {
		if (dimensions_changed) {
			set_dirty(yazu);
		}

		return;
	}

	if (!send_frame(yazu)) {
		fprintf(stderr, "failed to send first frame\n");
		yazu->failed = true;
		yazu->running = false;

		return;
	}

	setup_surface_frame_callback(yazu);
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
		send_frame(yazu);
		setup_surface_frame_callback(yazu);
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
	wl_surface_commit(yazu->wl_surface);
}

static void set_dirty(struct yazu *yazu) {
	if (!yazu->configured) {
		return;
	}

	yazu->dirty = true;
	if (yazu->surface_frame_callback) {
		return;
	}

	setup_surface_frame_callback(yazu);
}

static bool send_frame(struct yazu *yazu) {
	struct yazu_buffer *buffer;
	struct yazu_capture *capture = &yazu->capture;
	if (!capture->frame_ready) {
		buffer = get_available_buffer(yazu->wl_shm, yazu->buffers, 2, yazu->buffer_width, yazu->buffer_height);
		if (buffer == NULL) {
			return false;
		}

		memset(buffer->data, 0, buffer->size);

		goto commit_surface;
	}

	if (!approximately_equals(yazu->scale_x, yazu->scale_y)) {
		// 'scale_x' and 'scale_y' relate the surface size to the
		// capture buffer size. If the aspect ratio of the capture
		// buffer and surface are not equal it is assumed that this is
		// temporary and will be resolved by a future configure or
		// output transformation. Until then, let's just not render.
		goto set_dirty;
	}

	buffer = capture->buffer;
	if (buffer->busy) {
		return false;
	}

	wl_fixed_t viewport_x = wl_fixed_from_double(buffer_x_to_capture_x(yazu, 0));
	wl_fixed_t viewport_y = wl_fixed_from_double(buffer_y_to_capture_y(yazu, 0));
	wl_fixed_t viewport_width = wl_fixed_from_double(capture->buffer_width / real_zoom_scale(yazu));
	wl_fixed_t viewport_height = wl_fixed_from_double(capture->buffer_height / real_zoom_scale(yazu));
	wp_viewport_set_source(yazu->wp_viewport, viewport_x, viewport_y, viewport_width, viewport_height);
	wp_viewport_set_destination(yazu->wp_viewport, yazu->width, yazu->height);

commit_surface:
	buffer->busy = true;
	wl_surface_attach(yazu->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage(yazu->wl_surface, 0, 0, yazu->width, yazu->height);
	wl_surface_commit(yazu->wl_surface);

set_dirty:
	yazu->dirty = yazu->sliding || yazu->zooming;

	return true;
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

static bool clamp_capture_target(struct yazu *yazu) {
	bool clamped_x, clamped_y;
	clamped_x = clamp_capture_target_x(yazu);
	clamped_y = clamp_capture_target_y(yazu);

	return clamped_x || clamped_y;
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

static void recompute_dimensions(struct yazu *yazu) {
	struct yazu_capture *capture = &yazu->capture;
	if (capture->frame_ready) {
		yazu->buffer_width = capture->buffer_width;
		yazu->buffer_height = capture->buffer_height;
	} else {
		yazu->buffer_width = yazu->width;
		yazu->buffer_height = yazu->height;
	}
	yazu->half_buffer_width = yazu->buffer_width / 2.0f;
	yazu->half_buffer_height = yazu->buffer_height / 2.0f;
	yazu->scale_x = ((double) yazu->buffer_width) / yazu->width;
	yazu->scale_y = ((double) yazu->buffer_height) / yazu->height;
}

static void trim_old_mouse_samples(struct yazu_seat *seat, uint32_t time) {
	struct wl_array *motion_events = &seat->motion_events;

	struct yazu_mouse_sample *mouse_sample;
	uint32_t elapsed_time;
	for (size_t i = 0; i < motion_events->size; i += sizeof(struct yazu_mouse_sample)) {
		mouse_sample = motion_events->data + i;
		elapsed_time = time - mouse_sample->time;
		if (elapsed_time > SAMPLE_IS_OLD_THRESHOLD) {
			motion_events->size = i;

			break;
		}
	}
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

static bool approximately_equals(double a, double b) {
	return fabs(a - b) < 1.0e-6;
}

static void process_animations(struct yazu *yazu, uint32_t time) {
	process_zooming(yazu, time);
	process_sliding(yazu, time);
}


static void destroy_pointer(struct yazu_seat *seat) {
	if (seat->wl_pointer) {
		wl_pointer_destroy(seat->wl_pointer);
	}
	if (seat->wp_cursor_shape_device) {
		wp_cursor_shape_device_v1_destroy(seat->wp_cursor_shape_device);
	}
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
	for (EGLint i = 0; i < num_matching_configs; i++) {
		EGLConfig config = matching_configs[i];
		EGLint buffer_bpp, red_size, green_size, blue_size, alpha_size;
		eglGetConfigAttrib(yazu->egl.display, config, EGL_BUFFER_SIZE, &buffer_bpp);
		eglGetConfigAttrib(yazu->egl.display, config, EGL_RED_SIZE, &red_size);
		eglGetConfigAttrib(yazu->egl.display, config, EGL_GREEN_SIZE, &green_size);
		eglGetConfigAttrib(yazu->egl.display, config, EGL_BLUE_SIZE, &blue_size);
		eglGetConfigAttrib(yazu->egl.display, config, EGL_ALPHA_SIZE, &alpha_size);
		printf("buffer_bpp: %d, red_size: %d, green_size: %d, blue_size: %d, alpha_size: %d\n",
			buffer_bpp, red_size, green_size, blue_size, alpha_size);
	}
	yazu->egl.config = matching_configs[0];
	free(matching_configs);

	yazu->egl.context = eglCreateContext(yazu->egl.display, yazu->egl.config, EGL_NO_CONTEXT, context_attributes);
	assert(yazu->egl.context != EGL_NO_CONTEXT);
}

int main(int argc, char **argv) {
	bool ret_code = EXIT_FAILURE;
	struct yazu yazu = {
		.running = true,
		.scale_x = 1,
		.scale_y = 1,
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
	verify_global_object_exists(ext_image_copy_capture_manager);
	verify_global_object_exists(ext_output_image_capture_source_manager);
	verify_global_object_exists(zwlr_layer_shell);
	if (unsupported_compositor) {
		goto cleanup_bindings;
	}

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
	while (yazu.running && (num_dispatched = wl_display_dispatch(display)) != -1) {
	}
	ret_code = yazu.failed || (num_dispatched == -1);

	destroy_capture(&yazu.capture);
	zwlr_layer_surface_v1_destroy(yazu.layer_surface);
	wl_surface_destroy(yazu.wl_surface);
	wp_viewport_destroy(yazu.wp_viewport);
	destroy_buffer(yazu.buffers[0]);
	destroy_buffer(yazu.buffers[1]);

	if (yazu.surface_frame_callback) {
		wl_callback_destroy(yazu.surface_frame_callback);
	}

	EGLint result;
	result = eglTerminate(yazu.egl.display);
	assert(result == EGL_TRUE);
	result = eglReleaseThread();
	assert(result == EGL_TRUE);

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
		wl_array_release(&seat->motion_events);
		destroy_pointer(seat);
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
	destroy_global_object_if_exists(wl_shm,);
	destroy_global_object_if_exists(wl_compositor,);
	wl_registry_destroy(wl_registry);

	// ensure all queued requests have been received by server
	wl_display_roundtrip(display);

	wl_display_disconnect(display);

	return ret_code;
}
