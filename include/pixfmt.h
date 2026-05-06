#ifndef _PIXFMT_H
#define _PIXFMT_H

#include "yazu.h"

void reorder_bytes_to_big_endian_rgbx(uint32_t *data, size_t data_size,
		enum wl_shm_format shm_format);

bool client_is_shm_format_supported(enum wl_shm_format shm_format);

#endif
