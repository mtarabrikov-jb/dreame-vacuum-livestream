# Phase 2 — streaming during cleaning (Source B)

**Status: scaffold + reverse-engineering done; two steps left to make it produce video.**

Phase 1 gives you a safe stream from the dock and gets out of the camera's way during cleaning.
Phase 2 fills that gap **without a second camera open** (which reboots the W10), by tapping the
frames `ava` already publishes internally.

## The approach

During cleaning, `ava`'s `camera_streamer` publishes NV21 frames to its AI-navigation node over a
nanomsg bus at `ipc:///tmp/avamsg.socket`. `ava_cam_relay`:

1. connects to that bus (as a passive subscriber — **does not touch `/dev/video*`**),
2. filters the AI-camera message,
3. extracts the NV21 frame,
4. encodes it to H264 with the SoC's hardware encoder (CedarX `libvencoder.so`),
5. serves H264 on `127.0.0.1:6969` — the **same** socket Source A uses, so the supervisor swaps
   A↔B transparently and go2rtc never notices.

## What already works in `src/ava_cam_relay.c`

- Runtime `dlopen` of the robot's `libnanomsg.so.5` — so it cross-compiles with only `clang`.
- Bus connect + receive loop, CRC16-Modbus framing helper.
- A complete TCP server on `:6969` for go2rtc.
- `--dump` mode: prints length + first bytes + type of the first 40 bus messages, so you can
  **verify the subscription on-device** and identify the camera message.

Run it on the robot **while it is cleaning**:

```sh
/data/camstream/ava_cam_relay --dump
```

If you see a steady ~15 fps of large messages appear when cleaning starts, the tap works.

## The three TODOs (marked `[TODO-1/2/3]` in the source)

1. **`MSGTYPE_AVA_AI_CAMERA`** — the numeric `getMsgType` id of the AI camera message. Find it by
   correlating `--dump` output with cleaning start (the big, high-rate messages).
2. **`struct ava_cam_hdr`** — the exact byte offsets of width / height / stride / format / timestamp
   inside the payload, and where the NV21 bytes begin. NV21 total = `W*H*3/2`.
3. **`nv21_to_h264()`** — wire up CedarX `libvencoder.so` (`VideoEncCreate` / `VideoEncInit` with
   `nInputYuvFmt = VENC_PIXEL_YVU420SP` / `VideoEncodeOneFrame` / `GetOneBitstreamFrame`). Known-good
   params from `vacuumstreamer`'s `recorder.cfg`: `864x480 @15fps YVU420SP`.

See [`../docs/REVERSE_ENGINEERING.md`](../docs/REVERSE_ENGINEERING.md) for the bus format, message
wrapping (raknetmessage + CRC16), and camera topology that back these TODOs.

## Build

Toolchain (same as vacuumstreamer's Dockerfile):

```sh
apt install clang gcc-aarch64-linux-gnu        # clang target + aarch64 sysroot/crt
```

Then, from `phase2-cleaning/` (or `make build-phase2` at the repo root):

```sh
make pull-libs   # fetch the robot's libnanomsg.so.5 (needs ROBOT reachable)
make             # cross-compile ava_cam_relay for aarch64
```

`pull-libs` copies the robot's own `libnanomsg.so.5` so we link against it directly (no `dlopen`,
and no glibc symbol newer than the robot's 2.23). Output `ava_cam_relay` is picked up automatically
by the root `make stage`/`upload`, and once present on the robot the supervisor uses it as Source B.

> Phase 2 is optional: if the toolchain or robot isn't available, the root `make build` just skips it
> and Phase 1 still works fully.
