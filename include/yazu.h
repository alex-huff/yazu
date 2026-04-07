#ifndef _YAZU_H
#define _YAZU_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct yazu {
	struct wl_compositor *wl_compositor;
	struct wl_shm *wl_shm;
	struct zwlr_layer_shell_v1 *layer_shell;
};

#endif
