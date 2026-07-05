# Phase 2 — streaming during cleaning (Source B)

> ## ⛔ Result: not achievable on the W10 (hardware limit)
>
> This was fully reverse-engineered, built, and **tested exhaustively on the real robot** — both a
> passive tap and an active force. Conclusion: **the RGB camera cannot stream during cleaning on this
> model**, so there is nothing to show. Use Phase 1 (dock viewing).
>
> Evidence (see [`../docs/REVERSE_ENGINEERING.md`](../docs/REVERSE_ENGINEERING.md#on-device-test-result-the-real-blocker-the-rgb-camera-does-not-stream-during-cleaning)):
> - The hook is verified correct — at startup it captured real frames, shm decoded exactly
>   (`672x504`, `508032 = 672*504*3/2`, NV21).
> - During cleaning the real `GetImageFrame` returns 0 on every call (`okframes` flat); the SunxiCam
>   reports `state=3` but its `dqbuf` yields no buffers — the sensor produces nothing.
> - Forcing `SunxiCam::start()` (even `shutdown()`+`start()` 9× mid-clean) still yielded **zero
>   frames**.
> - During cleaning `ava` holds `/dev/video1`+`/dev/video2` (the ToF obstacle sensor), not
>   `/dev/video0`. The MR813's single ISP is dedicated to the ToF sensor while cleaning, so the RGB
>   camera physically can't run. That is also why the vendor only allows RGB monitoring when idle.
>
> This directory is kept as a documented dead end (the tap mechanism, the CedarX encoder, and the
> build are all real and correct — there is simply no cleaning-time stream to feed them).

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

## Why it's not shippable, and what a safe version would need

The blocker is **not** the encoder or the plumbing — it is the CPU copy in `ava`'s capture thread
faulting the ISP during cleaning (see the warning at the top). A safe Phase 2 must avoid touching the
frame with the CPU inside `ava`:

- **Zero-copy tap.** In the `GetImageFrame` hook, write only ~32 bytes of metadata to shm: the ISP
  buffer's physical/ION address (`ImageFrame+0x18`), dimensions, and a sequence number — no `memcpy`.
  The relay's CedarX encoder then DMA-imports that physical buffer directly (CedarX can encode from an
  ION/physical address). This removes both the latency and the cache eviction from the capture thread.
  It is fragile: `ava` requeues the buffer to the ISP right after `GetImageFrame` returns, so the
  encoder must import+consume it within the frame window or risk tearing. Not attempted on the owner's
  working robot.
- **Separate VIN scaler.** The SoC has 4 scalers; in principle a second read-only stream could be
  pulled without reconfiguring the sensor. Also unproven and risky (a naive second open reboots).

If someone does finish the encoder path, note the CedarX H264 **parameter indices**
(bitrate/framerate/GOP in `VideoEncSetParameter`, `src/vencoder.h`) are standard Allwinner enum values
not confirmed against this exact BSP. `--stats` mode isolates the tap from the encoder for bisecting.
