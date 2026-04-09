#include "yazu.h"
#include "buffer.h"
#include "pixfmt.h"

// BEGIN REGISTRY

static void registry_handle_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct yazu *yazu = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		yazu->wl_compositor = wl_registry_bind(wl_registry, name,
			&wl_compositor_interface, 6);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		yazu->wl_shm = wl_registry_bind(wl_registry, name,
			&wl_shm_interface, 2);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct yazu_output *output = calloc(1, sizeof(struct yazu_output));
		if (output == NULL) {
			fprintf(stderr, "failed to allocated output\n");
			yazu->failed = true;
			return;
		}
		struct wl_output *wl_output = wl_registry_bind(wl_registry,
			name, &wl_output_interface, 4);
		output->wl_output = wl_output;
		wl_list_insert(&yazu->outputs, &output->link);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		yazu->ext_image_copy_capture_manager = wl_registry_bind(
			wl_registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		yazu->ext_output_image_capture_source_manager = wl_registry_bind(
			wl_registry, name,
			&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		yazu->layer_shell = wl_registry_bind(wl_registry, name,
			&zwlr_layer_shell_v1_interface, 5);
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
	capture->frame_ready = true;
	if (capture->byte_order != DEFAULT_BYTE_ORDER) {
		reorder_bytes(capture->buffer->data, capture->buffer->size,
			capture->byte_order);
	}
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
	capture->buffer_width = width;
	capture->buffer_height = height;
}

static void ext_image_copy_capture_session_handle_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t format) {
	struct yazu_capture *capture = data;
	if (capture->has_shm_format) {
		return;
	}

	uint8_t byte_order = DEFAULT_BYTE_ORDER;
	cairo_format_t cairo_format = wl_shm_format_to_cairo(format, &byte_order);
	if (cairo_format == CAIRO_FORMAT_INVALID) {
		return;
	}

	capture->shm_format = format;
	capture->cairo_format = cairo_format;
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

	capture->buffer = create_buffer(yazu->wl_shm, capture->buffer_width, capture->buffer_height, capture->shm_format, capture->cairo_format);
	if (capture->buffer == NULL) {
		fprintf(stderr, "failed to create buffer for output frame\n");
		yazu->failed = true;
		yazu->running = false;
		return;
	}

	capture->ext_image_copy_capture_frame = ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(capture->ext_image_copy_capture_frame,
		&ext_image_copy_capture_frame_listener, capture);
	ext_image_copy_capture_frame_v1_attach_buffer(capture->ext_image_copy_capture_frame,
		capture->buffer->wl_buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(capture->ext_image_copy_capture_frame,
		0, 0, capture->buffer_width, capture->buffer_height);
	ext_image_copy_capture_frame_v1_capture(capture->ext_image_copy_capture_frame);
}

static void ext_image_copy_capture_session_handle_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *session) {
	struct yazu_capture *capture = data;
	struct yazu *yazu = wl_container_of(capture, yazu, capture);
	if (yazu->running && !capture->capture_started) {
		fprintf(stderr, "capture session closed before frame could be captured\n");
		yazu->failed = true;
		yazu->running = false;
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
	uint32_t capture_options = 0;
	// TODO make this configurable
	if (true) {
		capture_options |= EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;
	}
	struct ext_image_capture_source_v1 *output_source = ext_output_image_capture_source_manager_v1_create_source(
		yazu->ext_output_image_capture_source_manager, wl_output);
	yazu->capture.ext_image_copy_capture_session = ext_image_copy_capture_manager_v1_create_session(
		yazu->ext_image_copy_capture_manager, output_source, capture_options);
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
	yazu->scale = scale;
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

	yazu->configured = true;
	yazu->width = width;
	yazu->height = height;

	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	struct yazu_buffer *buffer = get_available_buffer(yazu->wl_shm, yazu->buffers, 2, yazu->width, yazu->height);
	if (buffer == NULL) {
		return;
	}
	buffer->busy = true;

	cairo_t *cairo = buffer->cairo;
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cairo, 1, 0, 1, 1);
	cairo_paint(cairo);

	wl_surface_attach(yazu->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage(yazu->wl_surface, 0, 0, yazu->width, yazu->height);
	wl_surface_commit(yazu->wl_surface);
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

static void destroy_capture(struct yazu_capture *capture) {
	if (capture->ext_image_copy_capture_frame) {
		ext_image_copy_capture_frame_v1_destroy(capture->ext_image_copy_capture_frame);
	}
	if (capture->ext_image_copy_capture_session) {
		ext_image_copy_capture_session_v1_destroy(capture->ext_image_copy_capture_session);
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
	};
	wl_list_init(&yazu.outputs);
	struct wl_registry *wl_registry = wl_display_get_registry(display);
	wl_registry_add_listener(wl_registry, &registry_listener, &yazu);

	// roundtrip for registry
	wl_display_roundtrip(display);

	if (yazu.failed) {
		return EXIT_FAILURE;
	}
	if (yazu.wl_compositor == NULL) {
		fprintf(stderr, "compositor doesn't support wl_compositor\n");
		return EXIT_FAILURE;
	}
	if (yazu.wl_shm == NULL) {
		fprintf(stderr, "compositor doesn't support wl_shm\n");
		return EXIT_FAILURE;
	}
	if (yazu.ext_image_copy_capture_manager == NULL) {
		fprintf(stderr, "compositor doesn't support ext-image-copy-capture\n");
		return EXIT_FAILURE;
	}
	if (yazu.ext_output_image_capture_source_manager == NULL) {
		fprintf(stderr, "compositor doesn't support ext-output-image-capture-source\n");
		return EXIT_FAILURE;
	}
	if (yazu.layer_shell == NULL) {
		fprintf(stderr, "compositor doesn't support zwlr_layer_shell_v1\n");
		return EXIT_FAILURE;
	}

	yazu.wl_surface = wl_compositor_create_surface(yazu.wl_compositor);
	wl_surface_add_listener(yazu.wl_surface, &surface_listener, &yazu);
	yazu.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		yazu.layer_shell, yazu.wl_surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "zoom");
	zwlr_layer_surface_v1_set_anchor(yazu.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
	zwlr_layer_surface_v1_set_keyboard_interactivity(yazu.layer_surface,
		false);
	zwlr_layer_surface_v1_set_exclusive_zone(yazu.layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(yazu.layer_surface,
		&layer_surface_listener, &yazu);
	wl_surface_commit(yazu.wl_surface);

	while (yazu.running && wl_display_dispatch(display) != -1) {
	}

	destroy_capture(&yazu.capture);
	struct yazu_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &yazu.outputs, link) {
		wl_list_remove(&output->link);
		wl_output_destroy(output->wl_output);
		free(output);
	}
	ext_image_copy_capture_manager_v1_destroy(yazu.ext_image_copy_capture_manager);
	ext_output_image_capture_source_manager_v1_destroy(yazu.ext_output_image_capture_source_manager);
	zwlr_layer_surface_v1_destroy(yazu.layer_surface);
	wl_surface_destroy(yazu.wl_surface);
	destroy_buffer(yazu.buffers[0]);
	destroy_buffer(yazu.buffers[1]);

	// allow surfaces to be unmapped
	wl_display_roundtrip(display);

	zwlr_layer_shell_v1_destroy(yazu.layer_shell);
	wl_compositor_destroy(yazu.wl_compositor);
	wl_shm_destroy(yazu.wl_shm);
	wl_registry_destroy(wl_registry);
	wl_display_disconnect(display);

	return yazu.failed;
}
