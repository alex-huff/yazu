#include "yazu.h"
#include "pixfmt.h"

static inline uint32_t right_shift(uint32_t in, int8_t amount) {
	if (amount >= 0) {
		return in >> amount;
	} else {
		return in << -amount;
	}
}

void reorder_bytes(uint32_t *data, size_t data_size, uint8_t byte_order) {
	int8_t byte_shift_0 = ((int8_t) ((byte_order & 0b00000011u) >> 0) - 0) * 8;
	int8_t byte_shift_1 = ((int8_t) ((byte_order & 0b00001100u) >> 2) - 1) * 8;
	int8_t byte_shift_2 = ((int8_t) ((byte_order & 0b00110000u) >> 4) - 2) * 8;
	for (size_t i = 0; i < data_size / sizeof(uint32_t); i++) {
		data[i] =
			(right_shift(data[i], byte_shift_0) & 0x000000FFu) |
			(right_shift(data[i], byte_shift_1) & 0x0000FF00u) |
			(right_shift(data[i], byte_shift_2) & 0x00FF0000u) |
			0xFF000000u;
	}
}

uint8_t client_is_shm_format_supported(enum wl_shm_format shm_format) {
	switch(shm_format) {
#if YAZU_LITTLE_ENDIAN == 1
	case WL_SHM_FORMAT_XRGB8888:
		return 0b01101100;

	case WL_SHM_FORMAT_XBGR8888:
		return 0b11100100;

	case WL_SHM_FORMAT_RGBX8888:
		return 0b00011011;

	case WL_SHM_FORMAT_BGRX8888:
		return 0b00111001;

	case WL_SHM_FORMAT_ARGB8888:
		return 0b01101100;

	case WL_SHM_FORMAT_ABGR8888:
		return 0b11100100;

	case WL_SHM_FORMAT_RGBA8888:
		return 0b00011011;

	case WL_SHM_FORMAT_BGRA8888:
		return 0b00111001;

#else
	case WL_SHM_FORMAT_XRGB8888:
		return 0b00111001;

	case WL_SHM_FORMAT_XBGR8888:
		return 0b00011011;

	case WL_SHM_FORMAT_RGBX8888:
		return 0b11100100;

	case WL_SHM_FORMAT_BGRX8888:
		return 0b01101100;

	case WL_SHM_FORMAT_ARGB8888:
		return 0b00111001;

	case WL_SHM_FORMAT_ABGR8888:
		return 0b00011011;

	case WL_SHM_FORMAT_RGBA8888:
		return 0b11100100;

	case WL_SHM_FORMAT_BGRA8888:
		return 0b01101100;

#endif
	default:
		return 0;
	}
}
