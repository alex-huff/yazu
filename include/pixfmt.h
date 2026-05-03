#ifndef _PIXFMT_H
#define _PIXFMT_H

#include "yazu.h"

#define DEFAULT_BYTE_ORDER 0b11100100

void reorder_bytes(uint32_t *data, size_t data_size, uint8_t byte_order);

uint8_t client_is_shm_format_supported(enum wl_shm_format shm_format);

#endif
