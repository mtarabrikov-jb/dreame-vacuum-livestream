# Phase 2 — streaming during cleaning (Source B)

**Status: fully reverse-engineered and implemented; deployable. The one part to confirm on real
hardware is the CedarX H264 parameter tuning (frame tap + NV21 layout are verified).**

Phase 1 streams from the dock and gets out of the camera's way during cleaning. Phase 2 fills the gap
**without a second camera open** (which reboots the W10), by tapping the frames `ava` already captures.

## How it actually works (the first design was wrong — see below)

`ava` is monolithic and delivers camera frames in-process; there is **no external bus** to subscribe
to (`ipc:///tmp/avamsg.socket` is never bound). So we tap frames **inside** `ava`:

```
ava + libcamtap.so ──NV21──▶ /tmp/camtap.shm ──▶ ava_cam_relay ──H264──▶ :6969 ──▶ go2rtc
   (LD_PRELOAD tap)          (tmpfs seqlock)      (CedarX encoder)
```

1. **`libcamtap.so`** is `LD_PRELOAD`'d into `ava`. `node_camera_streamer` calls, across the `.so`
   boundary, `sunxi_cam::SunxiCam::GetImageFrame(ImageFrame*)` in `libsunxicamera.so` — so we can
   interpose it. On each frame we read the NV21 pointer at `ImageFrame+0x20` (size `w*h*3/2`, dims from
   the `OpenCamera` hook) and `memcpy` it into a tmpfs seqlock buffer. Minimal and defensive: a bug
   here can't stall navigation, and the heavy work is out-of-process.
2. **`ava_cam_relay`** (separate process) reads the buffer, encodes NV21→H264 with the SoC hardware
   encoder (`libvencoder.so`, CedarX), and serves H264 on `127.0.0.1:6969` — the **same** socket
   Source A uses, so the supervisor swaps A↔B transparently.

Full derivation (disassembly, struct offsets, why the bus doesn't work) is in
[`../docs/REVERSE_ENGINEERING.md`](../docs/REVERSE_ENGINEERING.md).

## Files

- `src/libcamtap.c` — the in-ava tap (exports the two mangled `sunxi_cam::SunxiCam::*` hook symbols).
- `src/ava_cam_relay.c` — shm reader + CedarX H264 encoder + `:6969` server. `--stats` mode just
  reports the tap (fps/size) with no encoder — run it first to prove frames flow.
- `src/camtap_shm.h` — the seqlock buffer format shared by both.
- `src/vencoder.h` — the CedarX `VideoEnc*` API + struct layouts we call.
- `inject-ava.sh` — opt-in install/remove of the tap (bind-mount wrapper over `/usr/bin/ava`).

## Build

Reproducible, no host toolchain needed:

```sh
make docker      # builds libcamtap.so + ava_cam_relay for aarch64 in a pinned container
```

Or with a local cross toolchain (`apt install gcc-aarch64-linux-gnu libc6-dev-arm64-cross`):

```sh
make             # aarch64-linux-gnu-gcc
```

The build is pinned to debian:bullseye on purpose: its cross glibc keeps `dlopen` at `GLIBC_2.17`, so
the binaries run on the robot's glibc 2.23 (a newer base emits `dlopen@GLIBC_2.34` and fails).

## Deploy (OPT-IN — restarts ava)

From the repo root, after `make build && make upload`:

```sh
make install-phase2    # bind-mount the tap wrapper over /usr/bin/ava and RESTART ava
make phase2-status     # check the tap is loaded in ava
```

> This restarts `ava`, the robot's navigation process. Do it with the robot idle on the dock. If
> anything is wrong, `make uninstall-phase2` (or `inject-ava.sh remove` over SSH) restores stock ava.

Then start a cleaning job and, on the robot, verify frames are flowing:

```sh
/data/camstream/ava_cam_relay --stats     # should print "<w>x<h> <bytes>, N frames total"
```

Once `--stats` shows frames, the supervisor uses `ava_cam_relay` as Source B automatically while
cleaning.

## The remaining unknown

The frame tap and NV21 layout are verified by disassembly. The CedarX H264 **parameter indices**
(bitrate/framerate/GOP in `VideoEncSetParameter`) are the standard Allwinner enum values but weren't
confirmed against this exact BSP — if the H264 output is malformed, that's the first place to look
(`src/vencoder.h`). `--stats` mode isolates the tap from the encoder so you can bisect quickly.
