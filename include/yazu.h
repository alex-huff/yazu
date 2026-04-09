#ifndef _YAZU_H
#define _YAZU_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <cairo.h>

#include "buffer.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct yazu {
	struct wl_compositor *wl_compositor;
	struct wl_shm *wl_shm;
	struct ext_image_copy_capture_manager_v1 *ext_image_copy_capture_manager;
	struct ext_output_image_capture_source_manager_v1 *ext_output_image_capture_source_manager;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct yazu_buffer *buffers[2];
	struct wl_list outputs;
	bool running;
	bool configured;
	int32_t scale;
	uint32_t width, height;
};

struct yazu_output {
	struct wl_list link;
	struct wl_output *wl_output;
};

#endif
