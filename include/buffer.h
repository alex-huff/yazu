#ifndef _BUFFER_H
#define _BUFFER_H

#include "yazu.h"

struct yazu_buffer {
	uint32_t width, height;
	size_t size;

	void *data;
	struct wl_buffer *wl_buffer;

	bool busy;
};

struct yazu_buffer *create_buffer(struct wl_shm *wl_shm, uint32_t width,
		uint32_t height, enum wl_shm_format wl_fmt);

void destroy_buffer(struct yazu_buffer *buffer);

struct yazu_buffer *get_available_buffer(struct wl_shm *wl_shm,
		struct yazu_buffer **buffers, uint8_t num_buffers,
		uint32_t width, uint32_t height);

#endif
