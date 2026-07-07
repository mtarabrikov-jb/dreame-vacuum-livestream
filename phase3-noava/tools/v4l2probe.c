// v4l2probe - can we capture the OV8856 via plain V4L2 with ava stopped?
//
// For a /dev/videoN: QUERYCAP, ENUM_FMT, ENUM_FRAMESIZES, try S_FMT (default
// 672x504 NV21), REQBUFS(mmap)/QBUF/STREAMON, grab N frames, report bytesused
// and the fraction of non-zero bytes (real image vs empty/size-0). Supports
// single-planar and multi-planar (sunxi vin is often mplane).
//
// Usage: v4l2probe /dev/video0 [width height FOURCC]   e.g. v4l2probe /dev/video0 672 504 NV21
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}
static void p4cc(uint32_t f, char *s) { s[0]=f; s[1]=f>>8; s[2]=f>>16; s[3]=f>>24; s[4]=0; }

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    int W = argc > 4 ? atoi(argv[2]) : 672;
    int H = argc > 4 ? atoi(argv[3]) : 504;
    uint32_t FMT = v4l2_fourcc('N','V','2','1');
    if (argc > 4 && strlen(argv[4]) >= 4)
        FMT = v4l2_fourcc(argv[4][0], argv[4][1], argv[4][2], argv[4][3]);

    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { printf("%s: open: %s\n", dev, strerror(errno)); return 1; }

    struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { printf("%s: QUERYCAP: %s\n", dev, strerror(errno)); return 1; }
    uint32_t caps = cap.capabilities | cap.device_caps;
    int mp = !!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    int btype = mp ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    printf("== %s: driver=%s card=%s caps=0x%08x %s%s\n", dev, cap.driver, cap.card, caps,
           (caps & V4L2_CAP_VIDEO_CAPTURE) ? "CAPTURE " : "",
           mp ? "CAPTURE_MPLANE" : "");

    printf("-- formats:\n");
    for (int i = 0;; i++) {
        struct v4l2_fmtdesc fd0; memset(&fd0, 0, sizeof fd0); fd0.index = i; fd0.type = btype;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fd0) < 0) break;
        char c[5]; p4cc(fd0.pixelformat, c);
        printf("   [%d] %s  %s\n", i, c, fd0.description);
        for (int j = 0;; j++) {
            struct v4l2_frmsizeenum fs; memset(&fs, 0, sizeof fs); fs.index = j; fs.pixel_format = fd0.pixelformat;
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                printf("        %ux%u\n", fs.discrete.width, fs.discrete.height);
            else { printf("        %ux%u .. %ux%u (stepwise)\n", fs.stepwise.min_width, fs.stepwise.min_height,
                          fs.stepwise.max_width, fs.stepwise.max_height); break; }
            if (j > 8) break;
        }
    }

    struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt); fmt.type = btype;
    char want[5]; p4cc(FMT, want);
    if (mp) { fmt.fmt.pix_mp.width = W; fmt.fmt.pix_mp.height = H; fmt.fmt.pix_mp.pixelformat = FMT; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE; fmt.fmt.pix_mp.num_planes = 1; }
    else    { fmt.fmt.pix.width = W; fmt.fmt.pix.height = H; fmt.fmt.pix.pixelformat = FMT; fmt.fmt.pix.field = V4L2_FIELD_NONE; }
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { printf("-- S_FMT %s %dx%d: %s\n", want, W, H, strerror(errno)); return 2; }
    uint32_t gw, gh, gf, gsize;
    if (mp) { gw = fmt.fmt.pix_mp.width; gh = fmt.fmt.pix_mp.height; gf = fmt.fmt.pix_mp.pixelformat; gsize = fmt.fmt.pix_mp.plane_fmt[0].sizeimage; }
    else    { gw = fmt.fmt.pix.width; gh = fmt.fmt.pix.height; gf = fmt.fmt.pix.pixelformat; gsize = fmt.fmt.pix.sizeimage; }
    char got[5]; p4cc(gf, got);
    printf("-- S_FMT wanted %s %dx%d -> got %s %ux%u size=%u %s\n", want, W, H, got, gw, gh, gsize,
           (gf == FMT && gw == (uint32_t)W && gh == (uint32_t)H) ? "(exact)" : "(ADJUSTED)");

    if (getenv("ILLUM")) {   // turn on the IR illuminator (V4L2_CID_ILLUMINATORS_1/2) for the ToF sensor
        struct v4l2_control ic;
        ic.id = 0x00980925; ic.value = 1; int r1 = xioctl(fd, VIDIOC_S_CTRL, &ic);
        ic.id = 0x00980926; ic.value = 1; int r2 = xioctl(fd, VIDIOC_S_CTRL, &ic);
        printf("-- Illuminator 1=%s 2=%s\n", r1 ? strerror(errno) : "on", r2 ? strerror(errno) : "on");
    }

    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb); rb.count = 4; rb.type = btype; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { printf("-- REQBUFS: %s\n", strerror(errno)); return 3; }
    printf("-- REQBUFS granted %u buffers\n", rb.count);

    void *bufs[8]; size_t blen[8];
    for (uint32_t i = 0; i < rb.count; i++) {
        struct v4l2_buffer b; struct v4l2_plane pl; memset(&b, 0, sizeof b); memset(&pl, 0, sizeof pl);
        b.index = i; b.type = btype; b.memory = V4L2_MEMORY_MMAP;
        if (mp) { b.m.planes = &pl; b.length = 1; }
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { printf("-- QUERYBUF %u: %s\n", i, strerror(errno)); return 3; }
        size_t len = mp ? pl.length : b.length;
        off_t off = mp ? pl.m.mem_offset : b.m.offset;
        bufs[i] = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
        blen[i] = len;
        if (bufs[i] == MAP_FAILED) { printf("-- mmap %u: %s\n", i, strerror(errno)); return 3; }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { printf("-- QBUF %u: %s\n", i, strerror(errno)); return 3; }
    }

    if (xioctl(fd, VIDIOC_STREAMON, &btype) < 0) { printf("-- STREAMON: %s\n", strerror(errno)); return 4; }
    printf("-- STREAMON ok; capturing 15 frames:\n");

    for (int n = 0; n < 15; n++) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) { printf("   frame %2d: select %s\n", n, r == 0 ? "TIMEOUT (no data)" : strerror(errno)); if (n && r == 0) break; continue; }
        struct v4l2_buffer b; struct v4l2_plane pl; memset(&b, 0, sizeof b); memset(&pl, 0, sizeof pl);
        b.type = btype; b.memory = V4L2_MEMORY_MMAP; if (mp) { b.m.planes = &pl; b.length = 1; }
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { printf("   frame %2d: DQBUF %s\n", n, strerror(errno)); continue; }
        uint32_t used = mp ? pl.bytesused : b.bytesused;
        // count non-zero bytes in the first min(used, 64KB) to tell a real image from an empty buffer
        uint8_t *d = bufs[b.index]; size_t scan = used < 65536 ? used : 65536; size_t nz = 0;
        for (size_t k = 0; k < scan; k++) if (d[k]) nz++;
        printf("   frame %2d: buf %u bytesused=%u nonzero=%zu/%zu (%.0f%%) first=%02x %02x %02x %02x\n",
               n, b.index, used, nz, scan, scan ? 100.0 * nz / scan : 0.0, d[0], d[1], d[2], d[3]);
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { printf("   requeue %s\n", strerror(errno)); break; }
    }

    xioctl(fd, VIDIOC_STREAMOFF, &btype);
    for (uint32_t i = 0; i < rb.count; i++) munmap(bufs[i], blen[i]);
    close(fd);
    printf("== done %s\n", dev);
    return 0;
}
