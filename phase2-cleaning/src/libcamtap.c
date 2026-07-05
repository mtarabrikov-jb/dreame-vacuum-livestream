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
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "camtap_shm.h"

#define SYM_OPEN  "_ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii"
#define SYM_FRAME "_ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE"
#define SYM_START "_ZN9sunxi_cam8SunxiCam5startEiiiii"
#define SYM_CLOSE "_ZN9sunxi_cam8SunxiCam11CloseCameraEv"
#define SYM_RETURN "_ZN9sunxi_cam8SunxiCam16ReturnImageFrameEPNS_10ImageFrameE"
#define IMAGEFRAME_DATA_OFF 0x20
static void *g_sunxi_self = NULL;    // last SunxiCam 'this' seen in the hooks
typedef int (*close_fn)(void *);
typedef int (*return_fn)(void *, void *);
static void maybe_start_bg(void);    // starts the FORCE>=4 background thread lazily
// SunxiCam object layout (verified): self+0 = ctx pointer, self+8 = state (3=streaming)
#define SUNXI_CTX_OFF   0x00
#define SUNXI_STATE_OFF 0x08

typedef int (*open_fn)(void*, int, int, int, int, int);
typedef int (*frame_fn)(void*, void*);
typedef int (*start_fn)(void*, int, int, int, int, int);

enum { MODE_COPY = 0, MODE_META = 1 };

static int g_w = 0, g_h = 0;
static struct camtap_shm *g_shm = NULL;
static int g_mode = MODE_COPY, g_every = 1, g_yonly = 0, g_force = 0, g_grab = 0, g_ir = 0;
static int g_grabbed = 0;
static int g_cfg = 0;
static unsigned long long g_seen = 0;
// saved OpenCamera args, so we can call SunxiCam::start with the same params
static int g_a1 = 0, g_fourcc = 0, g_a3 = 0;
static uint64_t g_last_force_ns = 0;

static void camtap_cfg(void) {
	if (g_cfg) return;
	g_cfg = 1;
	const char *m = getenv("CAMTAP_MODE");
	if (m && strcmp(m, "meta") == 0) g_mode = MODE_META;
	const char *e = getenv("CAMTAP_EVERY"); if (e && atoi(e) > 0) g_every = atoi(e);
	const char *y = getenv("CAMTAP_YONLY"); if (y) g_yonly = atoi(y);
	const char *f = getenv("CAMTAP_FORCE"); if (f) g_force = atoi(f);
	const char *g = getenv("CAMTAP_GRAB"); if (g) g_grab = atoi(g);
	const char *ir = getenv("CAMTAP_IR"); if (ir) g_ir = atoi(ir);
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

// Cache of mmap'd V4L2 buffers (offset -> vaddr) so continuous IR capture does
// not mmap/munmap every frame.
#define CAMTAP_NMAP 16
static struct { uint32_t off, len; void *va; } g_maps[CAMTAP_NMAP];
static int g_nmaps = 0;
static void *camtap_map(int fd, uint32_t off, uint32_t len) {
	for (int i = 0; i < g_nmaps; i++) if (g_maps[i].off == off) return g_maps[i].va;
	if (g_nmaps >= CAMTAP_NMAP || len == 0) return NULL;
	void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)off);
	if (m == MAP_FAILED) return NULL;
	g_maps[g_nmaps].off = off; g_maps[g_nmaps].len = len; g_maps[g_nmaps].va = m;
	return g_maps[g_nmaps++].va;
}
static volatile uint64_t g_last_ir_ns = 0;   // last ToF(video1) frame time; fresh = cleaning

// --- crash-safe camera ownership ------------------------------------------
// ava (camera_streamer) and our RGB drive thread both use the SAME SunxiCam
// object. Concurrent / double-owner access deadlocked ava. Fix: ONE mutex
// serializes every real SunxiCam call (from the drive thread AND from ava, whose
// calls all pass through our interposed hooks), plus explicit ownership handoff.
static pthread_mutex_t g_cam_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_cs_active_ns = 0;    // last time ava touched the camera (via a hook)
enum { OWN_NONE = 0, OWN_DRIVE = 1, OWN_STREAMER = 2 };
static volatile int g_owner = OWN_NONE;
// True when ava is using (or about to use) the camera, so the drive thread must
// keep off it: recent camera_streamer hook activity, or ToF streaming (cleaning).
static int cs_busy(void) {
	uint64_t n = now_ns();
	return (g_cs_active_ns && n - g_cs_active_ns < 6000000000ull)
	    || (g_last_ir_ns   && n - g_last_ir_ns   < 2000000000ull);
}

// Continuously copy the raw ToF frame (video1) into the shm ring via the seqlock.
static void ir_publish(const void *data, uint32_t used) {
	if (!g_shm || !data || used == 0 || used > CAMTAP_MAX_FRAME) return;
	g_last_ir_ns = now_ns();
	g_shm->seq++; __sync_synchronize();
	g_shm->width = 224; g_shm->height = 1558; g_shm->size = used; g_shm->format = 100; // raw ToF u16
	g_shm->ts_ns = now_ns();
	memcpy(g_shm->data, data, used);
	g_shm->frames++;
	__sync_synchronize(); g_shm->seq++;
}

// --- diagnostic: intercept ioctl to see which /dev/videoN ava dequeues from ---
// VIDIOC_DQBUF is _IOWR('V',17,struct v4l2_buffer): type byte 'V'=0x56, nr=17.
// We match on (type,nr) so we don't depend on the struct size. v4l2_buffer has
// bytesused at offset 8 (u32). This answers "what streams during cleaning".
typedef int (*ioctl_fn)(int, unsigned long, void *);
static signed char g_fddev[4096];   // 0=unknown, -2=not-video, (num+1) for videoN
static int fd_videonum(int fd) {
	if (fd < 0 || fd >= (int)sizeof(g_fddev)) return -1;
	signed char c = g_fddev[fd];
	if (c != 0) return (c == -2) ? -1 : (c - 1);
	char path[64], tgt[128];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);
	if (n <= 0) return -1;                            // transient, don't cache
	tgt[n] = 0;
	int num = (strncmp(tgt, "/dev/video", 10) == 0) ? atoi(tgt + 10) : -1;
	g_fddev[fd] = (num >= 0 && num < 4) ? (signed char)(num + 1) : -2;
	return (num >= 0 && num < 4) ? num : -1;
}

int ioctl(int fd, unsigned long req, void *arg);
int ioctl(int fd, unsigned long req, void *arg) {
	static ioctl_fn real = NULL;
	if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
	int r = real ? real(fd, req, arg) : -1;
	if (r == 0 && ((req >> 8) & 0xff) == 0x56 && arg && g_shm) {
		unsigned nr = req & 0xff;
		if (nr == 17) {                              // VIDIOC_DQBUF
			int v = fd_videonum(fd);
			if (v >= 0 && v < 4) {
				g_shm->dqbuf[v]++;
				g_shm->dqbytes[v] = *(uint32_t *)((char *)arg + 8); // bytesused
				// continuous raw capture of the chosen device (default video1)
				if (g_ir && v == g_ir) {
					void *planes = *(void **)((char *)arg + 64);
					if (planes) {
						uint32_t used = *(uint32_t *)((char *)planes + 0);
						uint32_t len  = *(uint32_t *)((char *)planes + 4);
						uint32_t moff = *(uint32_t *)((char *)planes + 8);
						void *va = camtap_map(fd, moff, len ? len : used);
						if (va) ir_publish(va, used ? used : len);
					}
				}
				// grab one raw multiplanar frame from the requested device
				if (g_grab && !g_grabbed && v == g_grab) {
					// v4l2_buffer (mplane): m.planes @ +64; plane: bytesused@0, length@4, mem_offset@8
					void *planes = *(void **)((char *)arg + 64);
					if (planes) {
						uint32_t used = *(uint32_t *)((char *)planes + 0);
						uint32_t len  = *(uint32_t *)((char *)planes + 4);
						uint32_t moff = *(uint32_t *)((char *)planes + 8);
						uint32_t cap  = len ? len : used;
						if (cap > 0 && cap <= CAMTAP_MAX_FRAME) {
							void *m = mmap(NULL, cap, PROT_READ, MAP_SHARED, fd, (off_t)moff);
							if (m != MAP_FAILED) {
								memcpy(g_shm->data, m, cap);
								munmap(m, cap);
								g_shm->grab_used = used; g_shm->grab_len = len;
								g_shm->grab_off = moff;  g_shm->grab_dev = (uint32_t)v;
								g_grabbed = 1;
							}
						}
					}
				}
			}
		} else if (nr == 5 || nr == 4) {             // VIDIOC_S_FMT / G_FMT
			int v = fd_videonum(fd);
			if (v >= 0 && v < 4) memcpy((void *)g_shm->vfmt[v], arg, 64);
		}
	}
	return r;
}

int _ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii(void *self, int a1, int fourcc,
                                             int a3, int width, int height) {
	static open_fn real = NULL; static close_fn rclose = NULL;
	if (!real) real = (open_fn)dlsym(RTLD_NEXT, SYM_OPEN);
	if (!rclose) rclose = (close_fn)dlsym(RTLD_NEXT, SYM_CLOSE);
	camtap_cfg();
	if (width > 0 && height > 0) { g_w = width; g_h = height; }
	g_a1 = a1; g_fourcc = fourcc; g_a3 = a3;   // remember params for the drive thread
	g_sunxi_self = self;
	g_cs_active_ns = now_ns();                  // ava is claiming the camera now
	maybe_start_bg();
	if (!g_shm) camtap_init_shm();
	if (g_shm) { g_shm->width = (uint32_t)width; g_shm->height = (uint32_t)height; g_shm->opens++; }
	pthread_mutex_lock(&g_cam_lock);
	if (g_owner == OWN_DRIVE && rclose) { rclose(self); }   // take it back from the drive thread
	int r = real ? real(self, a1, fourcc, a3, width, height) : -1;
	g_owner = OWN_STREAMER;
	pthread_mutex_unlock(&g_cam_lock);
	return r;
}

// ava closing the camera -> ownership returns to nobody (drive thread may resume
// once ava has been quiet for a while).
int _ZN9sunxi_cam8SunxiCam11CloseCameraEv(void *self) {
	static close_fn real = NULL;
	if (!real) real = (close_fn)dlsym(RTLD_NEXT, SYM_CLOSE);
	camtap_cfg();
	g_cs_active_ns = now_ns();
	pthread_mutex_lock(&g_cam_lock);
	int r = real ? real(self) : 0;
	if (g_owner == OWN_STREAMER) g_owner = OWN_NONE;
	pthread_mutex_unlock(&g_cam_lock);
	return r;
}

// Variant B: when real GetImageFrame yields no frame, optionally force the
// SunxiCam into streaming by calling its real start() — but ONLY when it is safe:
// no context allocated yet (self[0]==NULL) and not already streaming (state!=3),
// and rate-limited. This avoids the double-calloc / state corruption that a
// blind start() would cause.
// Find the single AvaNodeCameraStreamer instance in ava's heap by its vtable
// pointer (object[0] == vtable_symbol + 16). Cached after the first success.
static void *find_streamer(void) {
	static void *cached = NULL;
	if (cached) return cached;                 // cache successes only (retry on miss)
	void *h = dlopen("/ava/lib/node_camera_streamer.so", RTLD_NOLOAD | RTLD_NOW);
	if (!h) h = dlopen("node_camera_streamer.so", RTLD_NOLOAD | RTLD_NOW);
	void *vt = h ? dlsym(h, "_ZTVN21avanodecamerastreamer21AvaNodeCameraStreamerE") : NULL;
	if (!vt) return NULL;                       // node not loaded yet -> retry later
	uintptr_t want = (uintptr_t)vt + 16;        // vtable pointer stored in an object
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return NULL;
	char line[300];
	unsigned long scanned = 0;
	while (fgets(line, sizeof line, f)) {
		uintptr_t lo, hi; char perms[8];
		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
		if (perms[0] != 'r' || perms[1] != 'w') continue;          // rw only
		if (strstr(line, "[stack]")) continue;                     // not on the stack
		if (hi - lo > 512ul * 1024 * 1024) continue;               // skip huge maps
		for (uintptr_t p = lo; p + sizeof(uintptr_t) <= hi; p += sizeof(uintptr_t)) {
			if (*(volatile uintptr_t *)p == want) { cached = (void *)p; fclose(f); return cached; }
		}
		scanned += (hi - lo);
		if (scanned > 512ul * 1024 * 1024) break;                  // safety cap
	}
	fclose(f);
	return NULL;
}

// Force level 3: set the AI-camera switch (streamer+0xa4 = 3) so camera_streamer
// runs its own proper enable path (media-ctl links / ISP config) — the thing a
// raw SunxiCam::start() can't reproduce.
static void force_ai_switch(void) {
	void *st = find_streamer();
	if (g_shm) g_shm->streamer = (uint64_t)(uintptr_t)st;
	if (!st) return;
	volatile int *sw = (volatile int *)((char *)st + 0xa4);
	if (g_shm && g_shm->sw_hits == 0) g_shm->sw_before = (uint32_t)*sw;
	*sw = 3;
	if (g_shm) g_shm->sw_hits++;
}

// Background thread (CAMTAP_FORCE>=4): proactively hold the AI-camera switch on,
// independent of the GetImageFrame hook (which stops being called once
// camera_streamer closes the RGB camera). This is how we keep RGB streaming on
// the dock. Started from a constructor at ava load.
static void *switch_thread(void *arg) {
	(void)arg;
	for (int i = 0; i < 40 && !find_streamer(); i++) sleep(1);   // wait for the node
	while (1) {
		if (!g_shm) camtap_init_shm();
		force_ai_switch();
		sleep(1);
	}
	return NULL;
}
// RGB drive thread (CAMTAP_FORCE=5): produce a continuous color stream on the
// dock by owning the SunxiCam ourselves — but ONLY while ava isn't using it, and
// with every SunxiCam call under g_cam_lock so we never race ava. Ownership is
// handed back to ava the instant it wants the camera (cs_busy()).
static void *drive_thread(void *arg) {
	(void)arg;
	frame_fn  real_gif    = (frame_fn) dlsym(RTLD_NEXT, SYM_FRAME);
	open_fn   real_open   = (open_fn)  dlsym(RTLD_NEXT, SYM_OPEN);
	close_fn  real_close  = (close_fn) dlsym(RTLD_NEXT, SYM_CLOSE);
	return_fn real_return = (return_fn)dlsym(RTLD_NEXT, SYM_RETURN);
	if (!real_gif || !real_open || !real_close || !real_return) return NULL;
	for (int i = 0; i < 60 && !(g_sunxi_self && g_w > 0); i++) sleep(1);
	uint8_t imgframe[64];
	int held = 0, miss = 0;
	while (1) {
		void *self = g_sunxi_self;
		if (!self || g_w <= 0) { sleep(1); continue; }
		pthread_mutex_lock(&g_cam_lock);
		if (cs_busy()) {                        // ava wants/uses the camera -> get off it
			if (held && g_owner == OWN_DRIVE) { real_close(self); g_owner = OWN_NONE; }
			held = 0; miss = 0;
			pthread_mutex_unlock(&g_cam_lock);
			usleep(150000);
			continue;
		}
		if (!held) {                            // acquire only if free
			if (g_owner != OWN_NONE) { pthread_mutex_unlock(&g_cam_lock); usleep(150000); continue; }
			real_open(self, g_a1, g_fourcc, g_a3, g_w, g_h);
			g_owner = OWN_DRIVE; held = 1; miss = 0;
			if (g_shm) g_shm->forced++;
		}
		memset(imgframe, 0, sizeof imgframe);   // one frame, still under the lock
		int r = real_gif(self, imgframe);
		if (r == 1) {
			camtap_publish(*(void **)((char *)imgframe + IMAGEFRAME_DATA_OFF), g_w, g_h);
			real_return(self, imgframe);        // qbuf -> keep the stream flowing
			miss = 0;
		} else if (++miss > 15) {               // stalled -> release, re-acquire fresh
			real_close(self); g_owner = OWN_NONE; held = 0; miss = 0;
		}
		pthread_mutex_unlock(&g_cam_lock);
		usleep(r == 1 ? 55000 : 120000);        // ~18 fps
	}
	return NULL;
}
// Start the background thread LAZILY from the first hook call (ava is fully
// initialized by then) — NOT from a constructor, where pthread_create + dlsym
// during ld.so init can deadlock and break ava's camera init.
static pthread_once_t g_bg_once = PTHREAD_ONCE_INIT;
static void start_bg(void) {
	pthread_t t;
	if (g_force == 4) pthread_create(&t, NULL, switch_thread, NULL);
	else if (g_force >= 5) pthread_create(&t, NULL, drive_thread, NULL);
}
static void maybe_start_bg(void) { if (g_force >= 4) pthread_once(&g_bg_once, start_bg); }


typedef int (*shutdown_fn)(void*);
static void maybe_force_start(void *self) {
	if (g_force >= 4) return;                    // handled by switch_thread
	if (g_force == 3) { force_ai_switch(); return; }
	static start_fn real_start = NULL;
	static shutdown_fn real_shutdown = NULL;
	if (!g_force || !self) return;
	void *ctx = *(void **)((char *)self + SUNXI_CTX_OFF);
	int state = *(volatile int *)((char *)self + SUNXI_STATE_OFF);
	if (g_shm) { g_shm->sunxi_state = (uint32_t)state; g_shm->sunxi_ctx = ctx ? 1 : 0; }
	// level 1 (safe): only start when nothing is set up. level 2+ (aggressive):
	// re-init even if state==3/ctx!=0 (shutdown first to avoid leaking the ctx).
	if (g_force < 2 && (ctx != NULL || state == 3)) return;
	uint64_t now = now_ns();
	uint64_t gap = (g_force >= 2) ? 3000000000ull : 1000000000ull; // slower when aggressive
	if (now - g_last_force_ns < gap) return;
	g_last_force_ns = now;
	if (!real_start) real_start = (start_fn)dlsym(RTLD_NEXT, SYM_START);
	if (!real_shutdown) real_shutdown = (shutdown_fn)dlsym(RTLD_NEXT, "_ZN9sunxi_cam8SunxiCam8shutdownEv");
	if (g_force >= 2 && real_shutdown && ctx != NULL) real_shutdown(self); // release old ctx
	if (real_start && g_w > 0 && g_h > 0) {
		real_start(self, g_a1, g_fourcc, g_a3, g_w, g_h);
		if (g_shm) g_shm->forced++;
	}
}

int _ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE(void *self, void *frame) {
	static frame_fn real = NULL;
	if (!real) real = (frame_fn)dlsym(RTLD_NEXT, SYM_FRAME);
	camtap_cfg();
	g_sunxi_self = self;
	g_cs_active_ns = now_ns();                 // ava is polling the camera
	maybe_start_bg();
	if (!g_shm) camtap_init_shm();
	if (g_shm) g_shm->calls++;
	// serialize with the drive thread so the two never touch SunxiCam at once
	pthread_mutex_lock(&g_cam_lock);
	int r = real ? real(self, frame) : 0;
	pthread_mutex_unlock(&g_cam_lock);
	if (r == 1) {
		if (g_shm) g_shm->okframes++;
		// legacy: when explicitly in copy mode (no drive thread) publish ava's own
		// frames; with FORCE=5 the drive thread owns RGB and this stays idle.
		if (g_force < 4 && g_mode == MODE_COPY && frame && g_w > 0 && g_h > 0)
			camtap_publish(*(void **)((char *)frame + IMAGEFRAME_DATA_OFF), g_w, g_h);
	} else if (g_force < 4) {
		maybe_force_start(self);
	}
	return r;
}
