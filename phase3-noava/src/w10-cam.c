// w10-cam - standalone RGB camera capture for DIRECT mode (no ava).
//
// Drives the OV8856 itself by dlopen-ing the vendor libsunxicamera.so and
// calling the C++ SunxiCam methods with our OWN self object (ava is dead, so
// the sensor is free). Reuses the exact bring-up path libcamtap's dock drive
// thread used, but standalone. Publishes 672x504 NV21 into /tmp/camtap.shm via
// the seqlock; ava_cam_relay + go2rtc consume it unchanged.
//
// OpenCamera args reverse-engineered from a running ava (libcamtap g_a1/g_fourcc/
// g_a3, cross-checked via g_w/g_h): a1=2 (/dev/video2), fourcc="NV21", a3=15.
//
// Env: CAM_INDEX(2) CAM_W(672) CAM_H(504) CAM_A3(15) CAM_LIB(/usr/lib/libsunxicamera.so)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include "camtap_shm.h"

// SunxiCam exported C++ symbols (mangled) in libsunxicamera.so
#define SYM_OPEN   "_ZN9sunxi_cam8SunxiCam10OpenCameraEiiiii"
#define SYM_FRAME  "_ZN9sunxi_cam8SunxiCam13GetImageFrameEPNS_10ImageFrameE"
#define SYM_RETURN "_ZN9sunxi_cam8SunxiCam16ReturnImageFrameEPNS_10ImageFrameE"
#define SYM_CLOSE  "_ZN9sunxi_cam8SunxiCam11CloseCameraEv"
#define IMAGEFRAME_DATA_OFF 0x20     // ImageFrame: NV21 virtual ptr at +0x20 (verified)

typedef int (*open_fn)(void *self, int a1, int fourcc, int a3, int w, int h);
typedef int (*frame_fn)(void *self, void *imgframe);
typedef int (*close_fn)(void *self);

static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static struct camtap_shm *open_shm(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return NULL;
    if (ftruncate(fd, sizeof(struct camtap_shm)) != 0) { close(fd); return NULL; }
    void *p = mmap(NULL, sizeof(struct camtap_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    struct camtap_shm *s = (struct camtap_shm *)p;
    s->magic = CAMTAP_MAGIC; s->seq = 0;
    return s;
}

static int envi(const char *k, int def) { const char *v = getenv(k); return v ? atoi(v) : def; }

int main(void) {
    int idx = envi("CAM_INDEX", 2), W = envi("CAM_W", 672), H = envi("CAM_H", 504), a3 = envi("CAM_A3", 15);
    const char *lib = getenv("CAM_LIB"); if (!lib) lib = "/usr/lib/libsunxicamera.so";
    const char *shmpath = getenv("CAM_SHM"); if (!shmpath) shmpath = CAMTAP_SHM_PATH;
    const char *fcc = getenv("CAM_FOURCC");
    uint32_t fourcc = (fcc && strlen(fcc) >= 4)
        ? ((uint32_t)fcc[0] | (fcc[1] << 8) | (fcc[2] << 16) | ((uint32_t)fcc[3] << 24))
        : ((uint32_t)'N' | ('V' << 8) | ('2' << 16) | ((uint32_t)'1' << 24));   // "NV21"
    // Frame size: NV21 default (W*H*3/2); override for raw formats (e.g. BG12 ToF
    // is 16-bit -> W*H*2). CAM_FORMAT is the camtap.shm tag (0=NV21, 100=raw ToF).
    size_t framesz = getenv("CAM_FRAMESZ") ? (size_t)strtoul(getenv("CAM_FRAMESZ"), NULL, 10)
                                           : (size_t)W * H * 3 / 2;
    uint32_t shmfmt = getenv("CAM_FORMAT") ? (uint32_t)atoi(getenv("CAM_FORMAT")) : 0;

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
    open_fn  Open   = (open_fn)  dlsym(h, SYM_OPEN);
    frame_fn Get    = (frame_fn) dlsym(h, SYM_FRAME);
    frame_fn Return = (frame_fn) dlsym(h, SYM_RETURN);
    close_fn Close  = (close_fn) dlsym(h, SYM_CLOSE);
    if (!Open || !Get || !Return || !Close) { fprintf(stderr, "dlsym: missing SunxiCam symbols\n"); return 1; }

    struct camtap_shm *shm = open_shm(shmpath);
    if (!shm) { fprintf(stderr, "shm %s: cannot open\n", shmpath); return 1; }
    if (framesz > CAMTAP_MAX_FRAME) { fprintf(stderr, "frame too big\n"); return 1; }

    // Our own SunxiCam object: {ctx_ptr@0, state@8}. 64 bytes zeroed is plenty.
    void *self = calloc(1, 64);
    fprintf(stderr, "w10-cam: OpenCamera(/dev/video%d, fourcc=%08x, a3=%d, %dx%d, framesz=%zu fmt=%u)\n",
            idx, fourcc, a3, W, H, framesz, shmfmt);
    int r = Open(self, idx, (int)fourcc, a3, W, H);
    if (r != 1) { fprintf(stderr, "OpenCamera failed (ret=%d) - pipeline not up standalone?\n", r); return 2; }
    fprintf(stderr, "w10-cam: streaming; writing %zuB to %s\n", framesz, shmpath);

    uint8_t imgframe[64];   // ImageFrame is 0x28; 64 is safe headroom
    uint64_t frames = 0, misses = 0;
    while (!g_stop) {
        memset(imgframe, 0, sizeof imgframe);
        int gr = Get(self, imgframe);
        void *data = *(void **)(imgframe + IMAGEFRAME_DATA_OFF);
        if (gr != 1 || !data) { misses++; usleep(5000); continue; }

        shm->seq++; __sync_synchronize();          // odd = write in progress
        shm->width = (uint32_t)W; shm->height = (uint32_t)H; shm->format = shmfmt;
        shm->size = (uint32_t)framesz; shm->ts_ns = now_ns();
        memcpy(shm->data, data, framesz);
        shm->frames++;
        __sync_synchronize(); shm->seq++;          // even = done

        Return(self, imgframe);
        if ((++frames % 60) == 0)
            fprintf(stderr, "w10-cam: %llu frames (%llu misses)\n",
                    (unsigned long long)frames, (unsigned long long)misses);
    }

    fprintf(stderr, "w10-cam: stopping, CloseCamera\n");
    Close(self);
    return 0;
}
