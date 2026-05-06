#include "yazu.h"
#include "pixfmt.h"

static inline uint32_t right_shift(uint32_t in, int8_t amount) {
	if (amount >= 0) {
		return in >> amount;
	} else {
		return in << -amount;
	}
}

static uint8_t big_endian_rgbx_byte_order(enum wl_shm_format shm_format) {
	switch(shm_format) {
#if YAZU_LITTLE_ENDIAN == 1
	case WL_SHM_FORMAT_XRGB8888:
		return 0b11000110;

	case WL_SHM_FORMAT_XBGR8888:
		return 0b11100100;

	case WL_SHM_FORMAT_RGBX8888:
		return 0b00011011;

	case WL_SHM_FORMAT_BGRX8888:
		return 0b00111001;

	case WL_SHM_FORMAT_ARGB8888:
		return 0b11000110;

	case WL_SHM_FORMAT_ABGR8888:
		return 0b11100100;

	case WL_SHM_FORMAT_RGBA8888:
		return 0b00011011;

	case WL_SHM_FORMAT_BGRA8888:
		return 0b00111001;
#else
	case WL_SHM_FORMAT_XRGB8888:
		return 0b01101100;

	case WL_SHM_FORMAT_XBGR8888:
		return 0b11100100;

	case WL_SHM_FORMAT_RGBX8888:
		return 0b00011011;

	case WL_SHM_FORMAT_BGRX8888:
		return 0b10010011;

	case WL_SHM_FORMAT_ARGB8888:
		return 0b01101100;

	case WL_SHM_FORMAT_ABGR8888:
		return 0b11100100;

	case WL_SHM_FORMAT_RGBA8888:
		return 0b00011011;

	case WL_SHM_FORMAT_BGRA8888:
		return 0b10010011;
#endif
	default:
		assert(false);
	}
}

// Is byte reordering enough? If the capture can be transparent, this doesn't
// work because the r, g, and b channels are premultiplied so we need to divide
// them by alpha before dropping the alpha channel.
void reorder_bytes_to_big_endian_rgbx(uint32_t *data, size_t data_size, enum wl_shm_format shm_format) {
	uint8_t byte_order = big_endian_rgbx_byte_order(shm_format);
	int8_t byte_shift_1 = ((int8_t) ((byte_order & 0b00001100u) >> 2) - 1) * 8;
	int8_t byte_shift_2 = ((int8_t) ((byte_order & 0b00110000u) >> 4) - 2) * 8;
#if YAZU_LITTLE_ENDIAN == 1
	int8_t byte_shift_0 = ((int8_t) ((byte_order & 0b00000011u) >> 0) - 0) * 8;
#else
	int8_t byte_shift_3 = ((int8_t) ((byte_order & 0b11000000u) >> 6) - 3) * 8;
#endif
	for (size_t i = 0; i < data_size / sizeof(uint32_t); i++) {
		data[i] =
			(right_shift(data[i], byte_shift_1) & 0x0000FF00u) |
			(right_shift(data[i], byte_shift_2) & 0x00FF0000u) |
#if YAZU_LITTLE_ENDIAN == 1
			(right_shift(data[i], byte_shift_0) & 0x000000FFu) |
			0xFF000000u;
#else
			(right_shift(data[i], byte_shift_3) & 0xFF000000u) |
			0x000000FFu;
#endif
	}
}

bool client_is_shm_format_supported(enum wl_shm_format shm_format) {
	switch(shm_format) {
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_RGBX8888:
	case WL_SHM_FORMAT_BGRX8888:
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_RGBA8888:
	case WL_SHM_FORMAT_BGRA8888:
		return true;
	default:
		return false;
	}
}
