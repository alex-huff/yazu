#include "yazu.h"
#include "buffer.h"
#include "pixfmt.h"

#define RETURN_IF_NOT_RUNNING if (!yazu->running) return;

static void setup_surface_frame_callback(struct yazu *yazu);

static void set_dirty(struct yazu *yazu);

static bool send_frame(struct yazu *yazu);

static void resize_splits(struct yazu *yazu);

static double buffer_x_to_capture_x(struct yazu *yazu, double buffer_x);

static double buffer_y_to_capture_y(struct yazu *yazu, double buffer_y);

static void clamp_capture_target(struct yazu *yazu);

static void recompute_dimensions(struct yazu *yazu);

// BEGIN POINTER

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	RETURN_IF_NOT_RUNNING

}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	RETURN_IF_NOT_RUNNING

}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	RETURN_IF_NOT_RUNNING

	yazu->cursor_x = wl_fixed_to_int(surface_x) * yazu->scale;
	yazu->cursor_y = wl_fixed_to_int(surface_y) * yazu->scale;
	if (yazu->dragging) {
		double cursor_x_capture_space = buffer_x_to_capture_x(yazu, yazu->cursor_x);
		double cursor_y_capture_space = buffer_y_to_capture_y(yazu, yazu->cursor_y);
		double cursor_grab_diff_x = yazu->capture_grab_x - cursor_x_capture_space;
		double cursor_grab_diff_y = yazu->capture_grab_y - cursor_y_capture_space;
		yazu->capture_target_x += cursor_grab_diff_x;
		yazu->capture_target_y += cursor_grab_diff_y;
		clamp_capture_target(yazu);
		set_dirty(yazu);
	}
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button,
		uint32_t button_state) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	RETURN_IF_NOT_RUNNING

	yazu->button_state = button_state;
	yazu->last_button = button;
	switch (button) {
	case BTN_LEFT:
		yazu->dragging = button_state == WL_POINTER_BUTTON_STATE_PRESSED;
		if (yazu->dragging) {
			yazu->capture_grab_x = buffer_x_to_capture_x(yazu, yazu->cursor_x);
			yazu->capture_grab_y = buffer_y_to_capture_y(yazu, yazu->cursor_y);
		}

		break;

	case BTN_RIGHT:
	case BTN_MIDDLE:
	default:
		yazu->running = false;

		break;
	}
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t fixed_value) {
	struct yazu_seat *seat = data;
	struct yazu *yazu = seat->yazu;

	RETURN_IF_NOT_RUNNING

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}

	double value = wl_fixed_to_double(fixed_value);
	double old_zoom_percent = yazu->zoom_percent;
	double capture_x_at_cursor = buffer_x_to_capture_x(yazu, yazu->cursor_x);
	double capture_y_at_cursor = buffer_y_to_capture_y(yazu, yazu->cursor_y);
	yazu->zoom_percent -= value * yazu->zoom_scale;
	yazu->zoom_percent = MAX(100, yazu->zoom_percent);
	if (yazu->zoom_percent != old_zoom_percent) {
		yazu->zoom_scale = yazu->zoom_percent / 100;
		double new_capture_x_at_cursor = buffer_x_to_capture_x(yazu, yazu->cursor_x);
		double new_capture_y_at_cursor = buffer_y_to_capture_y(yazu, yazu->cursor_y);
		yazu->capture_target_x -= (new_capture_x_at_cursor - capture_x_at_cursor);
		yazu->capture_target_y -= (new_capture_y_at_cursor - capture_y_at_cursor);
		clamp_capture_target(yazu);
		set_dirty(yazu);
	}
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

	RETURN_IF_NOT_RUNNING

	if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
		seat->wl_pointer = wl_seat_get_pointer(wl_seat);
		assert(seat->wl_pointer);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
};

// END SEAT

// BEGIN REGISTRY

static void registry_handle_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct yazu *yazu = data;

	RETURN_IF_NOT_RUNNING

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		yazu->wl_compositor = wl_registry_bind(wl_registry, name,
			&wl_compositor_interface, 3);
		assert(yazu->wl_compositor);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		yazu->wl_shm = wl_registry_bind(wl_registry, name,
			&wl_shm_interface, 1);
		assert(yazu->wl_shm);
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
		struct wl_output *wl_output = wl_registry_bind(wl_registry,
			name, &wl_output_interface, 1);
		assert(wl_output);
		output->wl_output = wl_output;
		wl_list_insert(&yazu->outputs, &output->link);
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
		yazu->layer_shell = wl_registry_bind(wl_registry, name,
			&zwlr_layer_shell_v1_interface, 3);
		assert(yazu->layer_shell);
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

	RETURN_IF_NOT_RUNNING

	capture->frame_ready = true;
	if (capture->byte_order != DEFAULT_BYTE_ORDER) {
		reorder_bytes(capture->buffer->data, capture->buffer->size,
			capture->byte_order);
	}
	uint32_t *capture_pixels = capture->buffer->data;
	uint32_t *padded_capture_pixels = capture->padded_image;
	for (uint32_t y = 0; y < capture->buffer_height; y++) {
		memcpy(
			padded_capture_pixels + y * (capture->buffer_width + 1),
			capture_pixels + y * capture->buffer_width,
			capture->buffer_width * sizeof(uint32_t));
	}
	set_dirty(yazu);
}

static void ext_image_copy_capture_frame_handle_failed(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);

	RETURN_IF_NOT_RUNNING

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

	// We right shift the capture buffer index 5 bits so (dimension - 1)
	// must be representable without using the top 5 bits.
	assert((width - 1) <= 0x07FFFFFF && (height - 1) <= 0x07FFFFFF);

	capture->buffer_width = width;
	capture->buffer_height = height;
	yazu->capture_target_x = capture->buffer_width / 2.0f;
	yazu->capture_target_y = capture->buffer_height / 2.0f;
}

static void ext_image_copy_capture_session_handle_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t shm_format) {
	struct yazu_capture *capture = data;
	if (capture->has_shm_format) {
		return;
	}

	uint8_t byte_order = is_shm_format_supported(shm_format);
	if (byte_order == 0) {
		return;
	}

	capture->shm_format = shm_format;
	capture->has_shm_format = true;
	capture->byte_order = byte_order;
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

	RETURN_IF_NOT_RUNNING

	if (capture->capture_started) {
		return;
	}

	capture->capture_started = true;
	if (!capture->has_shm_format) {
		fprintf(stderr, "no supported format found for output frame\n");
		yazu->failed = true;
		yazu->running = false;
		return;
	}

	capture->buffer = create_buffer(yazu->wl_shm, capture->buffer_width, capture->buffer_height, capture->shm_format);
	if (capture->buffer == NULL) {
		goto error;
	}

	capture->padded_image = calloc(capture->buffer_height + 1, capture->buffer_width + 1);
	if (capture->padded_image == NULL) {
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
	fprintf(stderr, "failed to create buffer for output frame\n");
	yazu->failed = true;
	yazu->running = false;
	return;
}

static void ext_image_copy_capture_session_handle_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *session) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);

	RETURN_IF_NOT_RUNNING

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

	RETURN_IF_NOT_RUNNING

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
	struct yazu *yazu = data;

	RETURN_IF_NOT_RUNNING

	assert(scale >= 1);
	// TODO: use fractional scale
	if (yazu->scale != scale) {
		yazu->scale = scale;
		recompute_dimensions(yazu);
		set_dirty(yazu);
	}
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

	RETURN_IF_NOT_RUNNING

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

	RETURN_IF_NOT_RUNNING

	wl_callback_destroy(wl_callback);
	yazu->surface_frame_callback = NULL;

	if (yazu->dirty) {
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
	struct yazu_capture *capture = &yazu->capture;
	uint32_t padded_capture_buffer_width = capture->buffer_width + 1;
	if (capture->frame_ready && (
			yazu->buffer_width != capture->buffer_width ||
			yazu->buffer_height != capture->buffer_height)) {
		// The capture dimensions should match the surface's buffer
		// dimensions unless we try to render a frame in between a
		// scale/transform update and a configure. If so, let's just
		// not render for now. We can unset the 'dirty' flag because
		// once dimensions become consistent as result of a surface
		// or output update, 'dirty' will have been set again by the
		// corresponding handler.
		goto unset_dirty;
	}

	struct yazu_buffer *buffer = get_available_buffer(yazu->wl_shm, yazu->buffers, 2, yazu->buffer_width, yazu->buffer_height);
	if (buffer == NULL) {
		return false;
	}

	if (!capture->frame_ready) {
		memset(buffer->data, 0, buffer->size);

		goto commit_surface;
	}

	resize_splits(yazu);
	uint32_t *h_splits = yazu->h_splits;
	uint32_t *v_splits = yazu->v_splits;
	uint32_t *split;
#define compute_splits(splits, buffer_dimension, translation_func, capture_dimension) \
	for (uint32_t i = 0; i < buffer_dimension; i++) { \
		split = splits + i; \
		double capture_start, capture_end; \
		capture_start = MAX(0, translation_func(yazu, i)); \
		capture_end = MIN(capture_dimension, translation_func(yazu, i + 1)); \
		assert(capture_start < capture_end); \
		*split = (uint32_t) floor(capture_start); \
		assert(*split >= 0 && *split < capture_dimension); \
		uint32_t barrier = *split + 1; \
		*split <<= 5; \
		if (barrier >= capture_end) { \
			*split |= 16; \
		} else { \
			*split |= lround( \
				((barrier - capture_start) / (capture_end - capture_start)) * 16); \
			assert((*split & 0b11111u) >= 0 && (*split & 0b11111u) <= 16); \
		} \
	}
	compute_splits(h_splits, yazu->buffer_width, buffer_x_to_capture_x, capture->buffer_width);
	compute_splits(v_splits, yazu->buffer_height, buffer_y_to_capture_y, capture->buffer_height);

/* #include <sys/time.h> */
/* struct timeval tval_before, tval_after, tval_result; */
/* gettimeofday(&tval_before, NULL); */

	uint32_t *pixels         = buffer->data;
	uint32_t *capture_pixels = capture->padded_image;
	uint32_t x, y;
	uint32_t v_split, h_split;
	uint64_t top, bottom;
	uint32_t top_left, top_right, bottom_left, bottom_right;
	uint8_t  h_first_prop, h_second_prop, v_first_prop, v_second_prop;
	uint16_t top_left_prop, top_right_prop, bottom_left_prop, bottom_right_prop;
	uint32_t buffer_row_index;
	uint32_t *capture_row_address;
	uint32_t *top_left_address;
	int32_t  last_v_first_pixel, last_h_first_pixel, v_first_pixel, h_first_pixel;
	bool     last_v_fully_contained, last_h_fully_contained, v_fully_contained, h_fully_contained;
	last_v_first_pixel     = -1;
	last_v_fully_contained = false;
	for (y = 0; y < yazu->buffer_height; y++) {
		v_split           = *(v_splits + y);
		v_first_prop      = (v_split & 0b11111u);
		v_second_prop     = 16 - v_first_prop;
		v_first_pixel     = v_split >> 5;
		v_fully_contained = v_first_prop == 16;
		buffer_row_index  = y * yazu->buffer_width;
		if (v_fully_contained && last_v_fully_contained && v_first_pixel == last_v_first_pixel) {
			memcpy(
				pixels + buffer_row_index,
				pixels + (buffer_row_index - yazu->buffer_width),
				yazu->buffer_width * sizeof(uint32_t));

			continue;
		}

		last_v_fully_contained = v_fully_contained;
		last_v_first_pixel     = v_first_pixel;
		capture_row_address    = capture_pixels + v_first_pixel * (padded_capture_buffer_width);
		last_h_first_pixel     = -1;
		last_h_fully_contained = false;
		for (x = 0; x < yazu->buffer_width; x++) {
			h_split           = *(h_splits + x);
			h_first_prop      = (h_split & 0b11111u);
			h_second_prop     = 16 - h_first_prop;
			h_first_pixel     = h_split >> 5;
			h_fully_contained = h_first_prop == 16;
			if (h_fully_contained && last_h_fully_contained && h_first_pixel == last_h_first_pixel) {
				pixels[buffer_row_index + x] = pixels[buffer_row_index + (x - 1)];

				continue;
			}

			last_h_fully_contained = h_fully_contained;
			last_h_first_pixel     = h_first_pixel;
			top_left_address       = capture_row_address + h_first_pixel;
			top                    = *((uint64_t *) top_left_address);
			bottom                 = *((uint64_t *) (top_left_address + padded_capture_buffer_width));
#if YAZU_LITTLE_ENDIAN
			top_left               = top    >> 0;
			top_right              = top    >> 32;
			bottom_left            = bottom >> 0;
			bottom_right           = bottom >> 32;
#else
			top_left               = top    >> 32;
			top_right              = top    >> 0;
			bottom_left            = bottom >> 32;
			bottom_right           = bottom >> 0;
#endif
			top_left_prop          = h_first_prop  * v_first_prop;
			top_right_prop         = h_second_prop * v_first_prop;
			bottom_left_prop       = h_first_prop  * v_second_prop;
			bottom_right_prop      = h_second_prop * v_second_prop;
#define extract_channel(pixel, channel_num) ((pixel >> (8 * (4 - channel_num))) & 0xFFu)
#define calculate_average_for_channel(channel_num) \
			( \
				top_left_prop     * extract_channel(top_left,     channel_num) + \
				top_right_prop    * extract_channel(top_right,    channel_num) + \
				bottom_left_prop  * extract_channel(bottom_left,  channel_num) + \
				bottom_right_prop * extract_channel(bottom_right, channel_num) \
			) >> 8
			pixels[buffer_row_index + x] =
				(calculate_average_for_channel(2) << 16) |
				(calculate_average_for_channel(3) << 8)  |
#if YAZU_LITTLE_ENDIAN
				(calculate_average_for_channel(4) << 0)  |
				0xFF000000;
#else
				(calculate_average_for_channel(1) << 24) |
				0x000000FF;
#endif
		}
	}

/* gettimeofday(&tval_after, NULL); */
/* timersub(&tval_after, &tval_before, &tval_result); */
/* printf("time elapsed: %ld.%06ld\n", tval_result.tv_sec, tval_result.tv_usec); */

commit_surface:
	buffer->busy = true;
	wl_surface_attach(yazu->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_set_buffer_scale(yazu->wl_surface, yazu->scale);
	wl_surface_damage(yazu->wl_surface, 0, 0, yazu->width, yazu->height);
	wl_surface_commit(yazu->wl_surface);

unset_dirty:
	yazu->dirty = false;

	return true;
}

static void recompute_dimensions(struct yazu *yazu) {
	yazu->buffer_width = yazu->width * yazu->scale;
	yazu->buffer_height = yazu->height * yazu->scale;
	yazu->half_buffer_width = yazu->buffer_width / 2.0f;
	yazu->half_buffer_height = yazu->buffer_height / 2.0f;
}

static void clamp_capture_target(struct yazu *yazu) {
	double buffer_top_capture_y = buffer_y_to_capture_y(yazu, 0);
	double buffer_bottom_capture_y = buffer_y_to_capture_y(yazu, yazu->buffer_height);
	if (buffer_top_capture_y < 0) {
		yazu->capture_target_y -= buffer_top_capture_y;
	} else if (buffer_bottom_capture_y > yazu->capture.buffer_height) {
		yazu->capture_target_y -= (buffer_bottom_capture_y - yazu->capture.buffer_height);
	}
	double buffer_left_capture_x = buffer_x_to_capture_x(yazu, 0);
	double buffer_right_capture_x = buffer_x_to_capture_x(yazu, yazu->buffer_width);
	if (buffer_left_capture_x < 0) {
		yazu->capture_target_x -= buffer_left_capture_x;
	} else if (buffer_right_capture_x > yazu->capture.buffer_width) {
		yazu->capture_target_x -= (buffer_right_capture_x - yazu->capture.buffer_width);
	}
}

static double buffer_x_to_capture_x(struct yazu *yazu, double buffer_x) {
	return yazu->capture_target_x + (buffer_x - yazu->half_buffer_width) / yazu->zoom_scale;
}

static double buffer_y_to_capture_y(struct yazu *yazu, double buffer_y) {
	return yazu->capture_target_y + (buffer_y - yazu->half_buffer_height) / yazu->zoom_scale;
}

static void destroy_splits(struct yazu *yazu) {
	if (yazu->v_splits) {
		free(yazu->v_splits);
	}
	if (yazu->h_splits) {
		free(yazu->h_splits);
	}
}

static void resize_splits(struct yazu *yazu) {
	if (yazu->buffer_width == yazu->h_splits_len && yazu->buffer_height == yazu->v_splits_len) {
		return;
	}

	destroy_splits(yazu);
	yazu->h_splits = calloc(yazu->buffer_width, sizeof(uint32_t));
	assert(yazu->h_splits);
	yazu->v_splits = calloc(yazu->buffer_height, sizeof(uint32_t));
	assert(yazu->v_splits);
	yazu->h_splits_len = yazu->buffer_width;
	yazu->v_splits_len = yazu->buffer_height;
}

static void destroy_capture(struct yazu_capture *capture) {
	if (capture->ext_image_copy_capture_frame) {
		ext_image_copy_capture_frame_v1_destroy(capture->ext_image_copy_capture_frame);
	}
	if (capture->ext_image_copy_capture_session) {
		ext_image_copy_capture_session_v1_destroy(capture->ext_image_copy_capture_session);
	}
	if (capture->padded_image) {
		free(capture->padded_image);
	}
	destroy_buffer(capture->buffer);
}

int main(int argc, char **argv) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to Wayland display\n");
		return EXIT_FAILURE;
	}
	struct yazu yazu = {
		.running = true,
		.scale = 1,
		.zoom_percent = 100,
		.zoom_scale = 1,
	};
	wl_list_init(&yazu.seats);
	wl_list_init(&yazu.outputs);
	struct wl_registry *wl_registry = wl_display_get_registry(display);
	assert(wl_registry);
	wl_registry_add_listener(wl_registry, &registry_listener, &yazu);

	// roundtrip for registry
	wl_display_roundtrip(display);

	bool ret_code = EXIT_FAILURE;
	if (yazu.wl_compositor == NULL) {
		fprintf(stderr, "compositor doesn't support wl_compositor\n");
		goto cleanup_bindings;
	}
	if (yazu.wl_shm == NULL) {
		fprintf(stderr, "compositor doesn't support wl_shm\n");
		goto cleanup_bindings;
	}
	if (yazu.ext_image_copy_capture_manager == NULL) {
		fprintf(stderr, "compositor doesn't support ext-image-copy-capture\n");
		goto cleanup_bindings;
	}
	if (yazu.ext_output_image_capture_source_manager == NULL) {
		fprintf(stderr, "compositor doesn't support ext-output-image-capture-source\n");
		goto cleanup_bindings;
	}
	if (yazu.layer_shell == NULL) {
		fprintf(stderr, "compositor doesn't support zwlr_layer_shell_v1\n");
		goto cleanup_bindings;
	}

	yazu.wl_surface = wl_compositor_create_surface(yazu.wl_compositor);
	assert(yazu.wl_surface);
	wl_surface_add_listener(yazu.wl_surface, &surface_listener, &yazu);
	yazu.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		yazu.layer_shell, yazu.wl_surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "zoom");
	assert(yazu.layer_surface);
	zwlr_layer_surface_v1_set_anchor(yazu.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
	zwlr_layer_surface_v1_set_keyboard_interactivity(yazu.layer_surface,
		0);
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
	destroy_splits(&yazu);
	destroy_buffer(yazu.buffers[0]);
	destroy_buffer(yazu.buffers[1]);

	if (yazu.surface_frame_callback) {
		wl_callback_destroy(yazu.surface_frame_callback);
	}

cleanup_bindings:
	if (yazu.layer_shell) {
		zwlr_layer_shell_v1_destroy(yazu.layer_shell);
	}
	if (yazu.ext_output_image_capture_source_manager) {
		ext_output_image_capture_source_manager_v1_destroy(yazu.ext_output_image_capture_source_manager);
	}
	if (yazu.ext_image_copy_capture_manager) {
		ext_image_copy_capture_manager_v1_destroy(yazu.ext_image_copy_capture_manager);
	}
	struct yazu_seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &yazu.seats, link) {
		wl_list_remove(&seat->link);
		if (seat->wl_pointer) {
			wl_pointer_destroy(seat->wl_pointer);
		}
		wl_seat_destroy(seat->wl_seat);
		free(seat);
	}
	struct yazu_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &yazu.outputs, link) {
		wl_list_remove(&output->link);
		wl_output_destroy(output->wl_output);
		free(output);
	}
	if (yazu.wl_shm) {
		wl_shm_destroy(yazu.wl_shm);
	}
	if (yazu.wl_compositor) {
		wl_compositor_destroy(yazu.wl_compositor);
	}
	wl_registry_destroy(wl_registry);

	// ensure all queued requests have been received by server
	wl_display_roundtrip(display);

	wl_display_disconnect(display);

	return ret_code;
}
