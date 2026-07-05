// ---------------------------------------------------------------------------
// libcamtap.so  —  in-ava camera frame tap  (Phase 2, Source B)
//
// LD_PRELOAD'd into the `ava` process. `ava`'s node_camera_streamer captures
// NV21 frames during cleaning by calling, ACROSS the .so boundary, into
// libsunxicamera.so:
//
//     sunxi_cam::SunxiCam::OpenCamera(self, a1, fourcc, a3, WIDTH, HEIGHT)
//     sunxi_cam::SunxiCam::GetImageFrame(self, ImageFrame* out)   // per frame
//
// Because those are exported symbols called cross-library, LD_PRELOAD can
// interpose them (exactly how tihmstar/vacuumstreamer hooks the Agora SDK).
// We wrap both:
//   * OpenCamera  -> remember WIDTH/HEIGHT (4th/5th args, verified by disasm).
//   * GetImageFrame -> after the real call returns 1 (success), the frame's NV21
//     virtual pointer is at *(void**)(ImageFrame + 0x20); size = w*h*3/2. We
//     copy it into a tmpfs seqlock buffer and return unchanged, so ava's
//     navigation is completely unaffected.
//
// The heavy work (H264 encode, network) happens in a SEPARATE process
// (ava_cam_relay) reading that buffer — so a bug there can never crash ava.
// This file does the bare minimum: one memcpy per frame, fully defensive.
//
// Struct offsets verified by disassembling libsunxicamera.so::GetImageFrame and
// node_camera_streamer.so — see docs/REVERSE_ENGINEERING.md.
// ---------------------------------------------------------------------------
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "camtap_shm.h"

// Mangled names of the interposed libsunxicamera.so symbols.
#define SYM_OPEN  "_ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii"
#define SYM_FRAME "_ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE"

// Offset of the NV21 data pointer inside sunxi_cam::ImageFrame (verified).
#define IMAGEFRAME_DATA_OFF 0x20

typedef int (*open_fn)(void*, int, int, int, int, int);
typedef int (*frame_fn)(void*, void*);

static int g_w = 0, g_h = 0;              // learned from OpenCamera
static struct camtap_shm *g_shm = NULL;   // tmpfs mapping

static void camtap_init_shm(void) {
	if (g_shm) return;
	int fd = open(CAMTAP_SHM_PATH, O_RDWR | O_CREAT, 0666);
	if (fd < 0) return;
	if (ftruncate(fd, sizeof(struct camtap_shm)) != 0) { close(fd); return; }
	void *p = mmap(NULL, sizeof(struct camtap_shm),
	               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) return;
	g_shm = (struct camtap_shm *)p;
	g_shm->magic = CAMTAP_MAGIC;
	g_shm->seq = 0;
	fprintf(stderr, "[camtap] shm ready at %s\n", CAMTAP_SHM_PATH);
}

static uint64_t now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void camtap_publish(const void *nv21, int w, int h) {
	if (!nv21 || w <= 0 || h <= 0) return;
	long size = (long)w * h * 3 / 2;
	if (size <= 0 || size > CAMTAP_MAX_FRAME) return;   // never overflow
	if (!g_shm) camtap_init_shm();
	if (!g_shm) return;

	// seqlock write: odd during update, even when consistent.
	g_shm->seq++;                 // -> odd
	__sync_synchronize();
	g_shm->width  = (uint32_t)w;
	g_shm->height = (uint32_t)h;
	g_shm->size   = (uint32_t)size;
	g_shm->format = 0;            // NV21
	g_shm->ts_ns  = now_ns();
	memcpy(g_shm->data, nv21, (size_t)size);
	g_shm->frames++;
	__sync_synchronize();
	g_shm->seq++;                 // -> even
}

// --- interposed: OpenCamera(self, a1, fourcc, a3, width, height) -----------
int _ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii(void *self, int a1, int fourcc,
                                             int a3, int width, int height) {
	static open_fn real = NULL;
	if (!real) real = (open_fn)dlsym(RTLD_NEXT, SYM_OPEN);
	if (width > 0 && height > 0) { g_w = width; g_h = height; }
	fprintf(stderr, "[camtap] OpenCamera %dx%d (fourcc=0x%08x)\n", width, height, fourcc);
	return real ? real(self, a1, fourcc, a3, width, height) : -1;
}

// --- interposed: GetImageFrame(self, ImageFrame* out) ----------------------
int _ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE(void *self, void *frame) {
	static frame_fn real = NULL;
	if (!real) real = (frame_fn)dlsym(RTLD_NEXT, SYM_FRAME);
	int r = real ? real(self, frame) : 0;
	if (r == 1 && frame && g_w > 0 && g_h > 0) {
		void *data = *(void **)((char *)frame + IMAGEFRAME_DATA_OFF);
		camtap_publish(data, g_w, g_h);
	}
	return r;
}
