#ifndef _YAZU_H
#define _YAZU_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <linux/input-event-codes.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "viewporter-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct yazu_input_sample {
	double x;
	double y;
	uint32_t time;
};

struct yazu_capture {
	struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session;
	struct ext_image_copy_capture_frame_v1 *ext_image_copy_capture_frame;

	struct yazu_buffer *buffer;

	bool has_shm_format;
	enum wl_shm_format shm_format;

	uint32_t buffer_width, buffer_height;

	bool capture_started;
	bool frame_ready;
};

struct yazu {
	struct wl_display *display;
	struct wl_compositor *wl_compositor;
	struct wl_shm *wl_shm;
	struct wp_viewporter *wp_viewporter;
	struct wp_cursor_shape_manager_v1 *wp_cursor_shape_manager;
	struct wp_single_pixel_buffer_manager_v1 *wp_single_pixel_buffer_manager;
	struct ext_image_copy_capture_manager_v1 *ext_image_copy_capture_manager;
	struct ext_output_image_capture_source_manager_v1 *ext_output_image_capture_source_manager;
	struct zwlr_layer_shell_v1 *zwlr_layer_shell;

	struct wl_array compositor_supported_shm_formats;

	struct wl_list seats;
	struct wl_list outputs;

	struct wl_surface *wl_surface;
	struct wp_viewport *wp_viewport;
	struct zwlr_layer_surface_v1 *layer_surface;

	struct wl_callback *surface_frame_callback;

	struct {
		EGLDisplay display;
		EGLContext context;
		EGLConfig config;
	} egl;
	struct wl_egl_window *wl_egl_window;
	EGLSurface *egl_surface;

	struct {
		GLuint vertex_position, texture_position;
	} gl;

	struct yazu_output *captured_output;
	struct yazu_capture capture;

	bool sliding;
	uint32_t slide_last_tick_time;
	double slide_x_acceleration;
	double slide_y_acceleration;
	double slide_x_velocity;
	double slide_y_velocity;

	bool zooming;
	uint32_t zoom_last_tick_time;
	double zoom_target_percent;
	struct yazu_seat *zoom_seat;

	bool gl_initialized;
	bool running;
	bool failed;
	bool configured;
	bool dirty;
	bool dragging;

	enum wl_output_transform transform;
	double buffer_scale_x, buffer_scale_y;
	uint32_t width, height;
	uint32_t buffer_width, buffer_height;
	double half_buffer_width, half_buffer_height;
	uint32_t transformed_buffer_width, transformed_buffer_height;
	double estimated_output_scale;
	double capture_target_x, capture_target_y;
	double zoom_scale, zoom_percent;
};

enum yazu_touch_event_type {
	DOWN,
	UP,
	MOTION,
};

struct yazu_touch_event {
	enum yazu_touch_event_type type;
	int32_t id;
	uint32_t time;
	wl_fixed_t x, y;
};

struct yazu_seat {
	struct yazu *yazu;
	struct wl_list link;

	struct wl_seat *wl_seat;

	double capture_grab_x, capture_grab_y;

	// pointer
	struct wl_pointer *wl_pointer;
	struct wp_cursor_shape_device_v1 *wp_cursor_shape_device;

	uint32_t last_button;
	enum wl_pointer_button_state button_state;

	double cursor_x, cursor_y;
	struct wl_array pointer_motion_events;

	bool pointer_on_surface;
	bool pointer_dragging;

	// touch
	struct wl_touch *wl_touch;
	struct wl_array wl_touch_events;
};

struct yazu_output {
	struct yazu *yazu;
	struct wl_list link;

	struct wl_output *wl_output;

	enum wl_output_transform transform;
};

#endif
