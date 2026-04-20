#include "yazu.h"
#include "buffer.h"

// https://wayland-book.com/print.html
static void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; i++) {
		buf[i] = 'A' + (r & 15) + (r & 16) * 2;
		r >>= 5;
	}
}

// https://wayland-book.com/print.html
static int create_shm_file() {
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		retries--;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);

			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

// https://wayland-book.com/print.html
static int allocate_shm_file(size_t size) {
	int fd = create_shm_file();
	if (fd < 0) {
		return -1;
	}

	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);

		return -1;
	}

	return fd;
}

// BEGIN BUFFER

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {
	struct yazu_buffer *buffer = data;
	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

// END BUFFER

bool client_is_shm_format_supported(enum wl_shm_format wl_shm_format) {
	switch (wl_shm_format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_RGBX8888:
	case WL_SHM_FORMAT_BGRX8888:
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_RGBA8888:
	case WL_SHM_FORMAT_BGRA8888:
		return true;
	default:
		return false;
	}
}

struct yazu_buffer *create_buffer(struct wl_shm *wl_shm, uint32_t width,
		uint32_t height, enum wl_shm_format wl_shm_format) {
	// only 32-bit 8888 formats are supported for now
	if (!client_is_shm_format_supported(wl_shm_format)) {
		return NULL;
	}

	uint32_t stride = width * sizeof(uint32_t);

	size_t size = stride * height;
	if (size == 0) {
		return NULL;
	}

	void *data = NULL;
	struct wl_buffer *wl_buffer = NULL;
	int fd = allocate_shm_file(size);
	if (fd < 0) {
		return NULL;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);

		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
	assert(pool);
	wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, wl_shm_format);
	assert(wl_buffer);
	struct yazu_buffer *buffer = calloc(1, sizeof(struct yazu_buffer));
	assert(buffer);
	buffer->width = width;
	buffer->height = height;
	buffer->size = size;
	buffer->data = data;
	buffer->wl_buffer = wl_buffer;

	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	return buffer;
}

void destroy_buffer(struct yazu_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}

	wl_buffer_destroy(buffer->wl_buffer);
	munmap(buffer->data, buffer->size);
	free(buffer);
}

struct yazu_buffer *get_available_buffer(struct wl_shm *wl_shm,
		struct yazu_buffer **buffers, uint8_t num_buffers,
		uint32_t width, uint32_t height) {
	size_t i;
	for (i = 0; i < num_buffers; i++) {
		if (buffers[i] == NULL || !buffers[i]->busy) {
			break;
		}
	}
	if (i == num_buffers) {
		return NULL;
	}

	if (buffers[i] && (buffers[i]->width != width || buffers[i]->height != height)) {
		destroy_buffer(buffers[i]);
		buffers[i] = NULL;
	}
	if (buffers[i] == NULL) {
		buffers[i] = create_buffer(wl_shm, width, height, WL_SHM_FORMAT_ARGB8888);
	}

	return buffers[i];
}
