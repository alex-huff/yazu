#ifndef _BUFFER_H
#define _BUFFER_H

#include "yazu.h"

struct yazu_buffer {
	uint32_t width, height;
	size_t size;
	void *data;
	struct wl_buffer *wl_buffer;
	cairo_surface_t *cairo_surface;
	cairo_t *cairo;
	bool busy;
};

struct yazu_buffer *create_buffer(struct wl_shm *wl_shm, uint32_t width,
		uint32_t height);
void destroy_buffer(struct yazu_buffer *buffer);

#endif
