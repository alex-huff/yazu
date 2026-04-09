#include "yazu.h"
#include "buffer.h"

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
			yazu->running = false;
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

// BEGIN SURFACE

static void surface_handle_enter(void *data, struct wl_surface *wl_surface,
		struct wl_output *wl_output) {
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

	if (!yazu.running) {
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

	struct yazu_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &yazu.outputs, link) {
		wl_list_remove(&output->link);
		wl_output_destroy(output->wl_output);
		free(output);
	}
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

	return 0;
}
