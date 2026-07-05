// ---------------------------------------------------------------------------
// Shared frame buffer between the in-ava tap (libcamtap.so) and the out-of-ava
// encoder/relay (ava_cam_relay). Backed by a file in tmpfs (/tmp -> RAM).
//
// Single-writer (the tap, inside ava), single-reader (the relay). We use a
// seqlock: the writer bumps `seq` to odd before touching the frame and to even
// after; the reader samples `seq` (must be even and unchanged across the copy).
// This means the tap NEVER blocks on the reader — critical, because it runs in
// the robot's navigation process and must not stall it.
// ---------------------------------------------------------------------------
#ifndef CAMTAP_SHM_H
#define CAMTAP_SHM_H

#include <stdint.h>

#define CAMTAP_SHM_PATH  "/tmp/camtap.shm"
#define CAMTAP_MAGIC     0x54504d43u          /* "CMPT" */

// Cap for one NV21 frame. AI camera is small (<= 1280x720); 1920x1088 is a
// generous ceiling. NV21 size = w*h*3/2.
#define CAMTAP_MAX_W     1920
#define CAMTAP_MAX_H     1088
#define CAMTAP_MAX_FRAME ((CAMTAP_MAX_W * CAMTAP_MAX_H * 3) / 2)

struct camtap_shm {
	volatile uint32_t magic;    // 0   CAMTAP_MAGIC once initialized
	volatile uint32_t seq;      // 4   seqlock: odd = write in progress
	volatile uint32_t width;    // 8
	volatile uint32_t height;   // 12
	volatile uint32_t size;     // 16  valid bytes in data[] (= w*h*3/2, or 0)
	volatile uint32_t format;   // 20  0 = NV21, 1 = Y-only(gray)
	volatile uint64_t frames;   // 24  frames published (with data)
	volatile uint64_t ts_ns;    // 32  CLOCK_MONOTONIC of last frame
	volatile uint64_t calls;    // 40  GetImageFrame hook entries (diagnostic)
	volatile uint64_t okframes; // 48  real GetImageFrame returned 1 (diagnostic)
	volatile uint64_t opens;    // 56  OpenCamera hook entries (diagnostic)
	volatile uint32_t sunxi_state; // 64  last observed SunxiCam state (self+8); 3=streaming
	volatile uint32_t sunxi_ctx;   // 68  1 if SunxiCam ctx (self[0]) allocated
	volatile uint64_t forced;   // 72  times we force-called SunxiCam::start
	volatile uint64_t dqbuf[4]; // 80,88,96,104  VIDIOC_DQBUF count per /dev/videoN
	volatile uint32_t dqbytes[4]; // 112,116,120,124  last bytesused per /dev/videoN
	uint8_t  vfmt[4][64];       // 128  raw v4l2_format bytes from S_FMT per /dev/videoN
	uint8_t  data[CAMTAP_MAX_FRAME]; // 384
};

#endif
