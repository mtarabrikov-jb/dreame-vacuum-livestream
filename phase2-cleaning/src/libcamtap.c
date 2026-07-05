// ---------------------------------------------------------------------------
// libcamtap.so  —  in-ava camera frame tap  (Phase 2, Source B)
//
// LD_PRELOAD'd into the `ava` process. `ava`'s node_camera_streamer captures
// NV21 frames by calling, ACROSS the .so boundary, into libsunxicamera.so:
//     sunxi_cam::SunxiCam::OpenCamera(self, a1, fourcc, a3, WIDTH, HEIGHT)
//     sunxi_cam::SunxiCam::GetImageFrame(self, ImageFrame* out)   // per frame
// Those are exported symbols called cross-library, so LD_PRELOAD interposes them
// (same trick as tihmstar/vacuumstreamer on the Agora SDK). The NV21 virtual
// pointer is at ImageFrame+0x20; size = w*h*3/2 (verified by disassembly).
//
// IMPORTANT on-device finding: a full 508 KB memcpy per frame INSIDE ava's
// capture thread destabilizes the VIN/ISP during cleaning (continuous
// sunxi_isp_reset, navigation blind). So the amount of work done here is
// configurable at runtime (no rebuild) via env vars, to find a cost the capture
// pipeline tolerates:
//
//   CAMTAP_MODE=meta   only publish metadata (dims/seq) — NO frame copy.     [diagnostic]
//   CAMTAP_MODE=copy   copy the frame into the shm buffer.                   [default]
//   CAMTAP_EVERY=N     in copy mode, copy only every Nth frame (default 1).
//   CAMTAP_YONLY=1     copy only the Y (luma) plane -> grayscale, 2/3 size.
//   CAMTAP_MAXW / CAMTAP_MAXH  ignore frames larger than this (safety).
//
// Whatever the mode, the tap is passive: it calls the real function and returns
// its result unchanged, so ava's navigation logic is never altered.
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

#define SYM_OPEN  "_ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii"
#define SYM_FRAME "_ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE"
#define IMAGEFRAME_DATA_OFF 0x20

typedef int (*open_fn)(void*, int, int, int, int, int);
typedef int (*frame_fn)(void*, void*);

enum { MODE_COPY = 0, MODE_META = 1 };

static int g_w = 0, g_h = 0;
static struct camtap_shm *g_shm = NULL;
static int g_mode = MODE_COPY, g_every = 1, g_yonly = 0;
static int g_cfg = 0;
static unsigned long long g_seen = 0;

static void camtap_cfg(void) {
	if (g_cfg) return;
	g_cfg = 1;
	const char *m = getenv("CAMTAP_MODE");
	if (m && strcmp(m, "meta") == 0) g_mode = MODE_META;
	const char *e = getenv("CAMTAP_EVERY"); if (e && atoi(e) > 0) g_every = atoi(e);
	const char *y = getenv("CAMTAP_YONLY"); if (y) g_yonly = atoi(y);
}

static void camtap_init_shm(void) {
	if (g_shm) return;
	int fd = open(CAMTAP_SHM_PATH, O_RDWR | O_CREAT, 0666);
	if (fd < 0) return;
	if (ftruncate(fd, sizeof(struct camtap_shm)) != 0) { close(fd); return; }
	void *p = mmap(NULL, sizeof(struct camtap_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) return;
	g_shm = (struct camtap_shm *)p;
	g_shm->magic = CAMTAP_MAGIC;
	g_shm->seq = 0;
}

static uint64_t now_ns(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void camtap_publish(const void *nv21, int w, int h) {
	if (w <= 0 || h <= 0) return;
	long full = (long)w * h * 3 / 2;
	if (full <= 0 || full > CAMTAP_MAX_FRAME) return;
	if (!g_shm) camtap_init_shm();
	if (!g_shm) return;

	long copysz = 0;
	if (g_mode == MODE_COPY && nv21) {
		if ((g_seen % (unsigned)g_every) == 0)
			copysz = g_yonly ? (long)w * h : full;   // Y-only = grayscale
	}

	g_shm->seq++;                 // -> odd (write in progress)
	__sync_synchronize();
	g_shm->width  = (uint32_t)w;
	g_shm->height = (uint32_t)h;
	g_shm->format = (uint32_t)(g_yonly ? 1 : 0);   // 0=NV21, 1=Y-only(gray)
	g_shm->ts_ns  = now_ns();
	g_shm->size   = (uint32_t)copysz;
	if (copysz > 0) memcpy(g_shm->data, nv21, (size_t)copysz);
	g_shm->frames++;
	__sync_synchronize();
	g_shm->seq++;                 // -> even
}

int _ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii(void *self, int a1, int fourcc,
                                             int a3, int width, int height) {
	static open_fn real = NULL;
	if (!real) real = (open_fn)dlsym(RTLD_NEXT, SYM_OPEN);
	camtap_cfg();
	if (width > 0 && height > 0) { g_w = width; g_h = height; }
	if (!g_shm) camtap_init_shm();
	if (g_shm) { g_shm->width = (uint32_t)width; g_shm->height = (uint32_t)height; g_shm->opens++; }
	return real ? real(self, a1, fourcc, a3, width, height) : -1;
}

int _ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE(void *self, void *frame) {
	static frame_fn real = NULL;
	if (!real) real = (frame_fn)dlsym(RTLD_NEXT, SYM_FRAME);
	camtap_cfg();
	if (!g_shm) camtap_init_shm();
	if (g_shm) g_shm->calls++;                 // every hook entry (diagnostic)
	int r = real ? real(self, frame) : 0;
	if (r == 1) {
		if (g_shm) g_shm->okframes++;          // real delivered a frame
		if (frame && g_w > 0 && g_h > 0) {
			g_seen++;
			const void *data = (g_mode == MODE_COPY)
				? *(void **)((char *)frame + IMAGEFRAME_DATA_OFF) : NULL;
			camtap_publish(data, g_w, g_h);
		}
	}
	return r;
}
