# In-`ava` camera shim + relay (the working feed)

This directory holds the actual camera pipeline: a single `LD_PRELOAD` shim injected into `ava` plus an
out-of-process relay. Together with the robot's go2rtc they produce one auto-switching stream — **color
RGB on the dock, infrared while cleaning** — without a cloud, without `video_monitor`, and without
destabilizing navigation.

> The long road here (the RGB camera won't stream during cleaning; the ToF stream is the only live feed
> then; the vendor `video_monitor` idles without a cloud trigger; CedarX H264 was too fragile; software
> JPEG won) is documented in [`../docs/REVERSE_ENGINEERING.md`](../docs/REVERSE_ENGINEERING.md).

## How it works

```
ava + libcamtap.so ──┬─ dock  : drive thread owns SunxiCam → NV21 672x504 (fmt 0)
                     └─ clean : ioctl DQBUF tap of /dev/video1 → raw ToF 224x1558 (fmt 100)
                              │  (one mutex + ownership handoff = never clashes with ava)
                              ▼
                     /tmp/camtap.shm ──▶ ava_cam_relay ──MJPEG──▶ 127.0.0.1:8090 ──▶ go2rtc
                     (tmpfs seqlock)     JPEG: color / IR-as-YCbCr
```

- **`libcamtap.so`** interposes the exported `sunxi_cam::SunxiCam::{OpenCamera,GetImageFrame,CloseCamera}`
  and libc `ioctl`.
  - On the dock a background thread drives the RGB camera itself (`OpenCamera` → `GetImageFrame` +
    `ReturnImageFrame` loop at ~14 fps — the `ReturnImageFrame`/qbuf is essential or the 3-buffer pool
    starves after ~3 frames) and copies each NV21 frame to the shm buffer.
  - While cleaning the `ioctl(VIDIOC_DQBUF)` hook copies the ToF frames `ava` is already dequeuing from
    `/dev/video1`.
  - **Crash-safety:** every real `SunxiCam` call (drive thread + `ava`'s, via the hooks) is serialized
    by one mutex, with explicit ownership handoff, so the two never fight over the camera. The drive
    thread yields the instant `ava` wants the camera (`cs_busy()` = recent hook activity or live ToF).
- **`ava_cam_relay`** reads the shm frame and encodes it with a self-contained baseline JPEG encoder,
  picking by `format`: `0` → NV21 color (4:2:0), `100` → ToF → grayscale, rotated 180°, encoded as
  **YCbCr with neutral chroma** (a 1-component grayscale JPEG makes go2rtc's MJPEG transcoder panic).
  It serves MJPEG on loopback `:8090`. Runs outside `ava`, so a bug here can't crash navigation.
- **go2rtc** consumes `:8090` and serves the stream `camera` as RTSP / WebRTC / MSE / snapshots.

## Files

- `src/libcamtap.c` — the in-`ava` shim (RGB drive thread, ToF `ioctl` tap, mutex ownership).
- `src/ava_cam_relay.c` — shm reader + JPEG encode + MJPEG `:8090` server. `--stats` reports the tap
  (fps/size/format) without encoding.
- `src/camtap_shm.h` — the seqlock buffer format.
- `src/ir_process.h` — ToF `224x1558` → grayscale (unstack 9 sub-frames, max-project, flat-field,
  contrast stretch, 180° rotate).
- `src/jpeg_gray.h` — the baseline JPEG encoder (color NV21→YCbCr 4:2:0, and a grayscale path).
- `run_ir.sh`, `go2rtc_ir.yaml` — start the relay + go2rtc; go2rtc config for stream `camera`.
- `inject-ava.sh` — inject/remove the shim (bind-mount wrapper over `/usr/bin/ava`) + boot persistence.

## Env (`camtap.env`, sourced by the ava wrapper)

- `CAMTAP_FORCE=5` — enable the RGB drive thread (dock color). `0` = passive only (no dock RGB).
- `CAMTAP_IR=1` — enable the ToF tap (cleaning infrared).
- relay: `IR_SCALE` (2), `IR_BAND` (-1 = max-project), `IR_QUALITY` (80), `IR_PORT` (8090).

## Build

Reproducible, no host toolchain needed (needs Docker):

```sh
make docker      # builds libcamtap.so + ava_cam_relay for aarch64 in a pinned container
```

Or with a local cross toolchain (`apt install gcc-aarch64-linux-gnu libc6-dev-arm64-cross`): `make`.
Pinned to debian:bullseye on purpose — its cross glibc keeps `dlopen`/pthread at `GLIBC_2.17`, so the
binaries run on the robot's glibc 2.23 (a newer base emits `@GLIBC_2.34` and fails).

## Deploy

From the repo root: `make build && make upload && make install` (restarts `ava` — do it with
the robot idle on the dock), then `make watch` for the URLs. Verify the tap on the robot with
`/data/camstream/ava_cam_relay --stats` (prints `<w>x<h> fmt=<0|100>` as the state changes).
