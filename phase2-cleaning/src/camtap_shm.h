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
	volatile uint32_t magic;   // CAMTAP_MAGIC once initialized
	volatile uint32_t seq;     // seqlock: odd = write in progress
	volatile uint32_t width;
	volatile uint32_t height;
	volatile uint32_t size;    // valid bytes in data[] (= w*h*3/2)
	volatile uint32_t format;  // 0 = NV21 (YVU420SP)
	volatile uint64_t frames;  // total frames published (monotonic)
	volatile uint64_t ts_ns;   // CLOCK_MONOTONIC of capture
	uint8_t  data[CAMTAP_MAX_FRAME];
};

#endif
