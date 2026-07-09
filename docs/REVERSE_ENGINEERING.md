# Reverse-engineering notes (Dreame W10 / `r2104`)

Everything below was determined on a rooted **Dreame Bot W10**, product code **`r2104`**. Other
Dreame models share the software stack (`ava`, CedarX, Valetudo-on-top) but differ in sensor,
product code, and struct layouts. Treat model-specific numbers as W10-only unless you re-verify.

Placeholders: `<robot-ip>` = your robot's LAN address; `root@<robot-ip>` = its SSH login.

> **Outcome (read this first).** This is a *log* — it records several hypotheses that were tried and
> discarded before the working solution, so individual sections below reach conclusions that a *later*
> section overturns. The shipped result:
> - **On the dock:** full-color RGB, by driving `ava`'s own RGB camera from a thread inside `ava`
>   (`SunxiCam` OpenCamera → GetImageFrame + **ReturnImageFrame**), serialized with `ava` by a mutex.
>   The RGB "black frame" scare was just a dark room.
> - **While cleaning:** grayscale infrared, by passively tapping the ToF sensor (`/dev/video1`) that
>   `ava` runs for obstacle avoidance. The RGB camera does **not** stream while cleaning — but the cause
>   is the **spinning LDS turret** wedging its ISP (isp0), not an "active-mode" firmware gate (that
>   reading is superseded — see §"Correction" below). Infrared is what's available while the turret
>   spins.
> - **Encode/serve:** software baseline JPEG (color, and grayscale-as-YCbCr for IR) → MJPEG → the
>   robot's go2rtc. The CedarX H264 path (sections below) was abandoned as too fragile.
> See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the final design.

---

## 1. Platform

- Model `r2104`, SoC **Allwinner MR813** (aarch64), Linux `4.9.191`, glibc `2.23`.
- Root filesystem: **read-only squashfs**. `/data` is the only writable, persistent fs (ext4).
  Everything we install lives under `/data`.
- Main controller: the **`ava`** daemon (`ava -f /ava/conf/r2104.conf force`). Robot behavior is a
  BehaviorTree.CPP v3 tree; nodes are `/ava/lib/node_*.so`.
- De-cloud layer: **Valetudo** runs *on top of* stock firmware (it does not replace navigation or
  docking). REST API at `http://127.0.0.1/api/v2` on the robot; web UI on `:80`.

## 2. Rooting / boot hook

Root is via the Dennis Giese / dustbuilder method. The relevant hook for us:
`/etc/rc.sysinit` runs, near its end,

```
if [[ -f /data/_root_postboot.sh ]]; then /data/_root_postboot.sh & fi
```

so `/data/_root_postboot.sh` is the user-controlled boot script (it normally disables WiFi
power-save, sets the timezone, and launches Valetudo). We append a **marked block** to it for
persistence and remove that block on uninstall — never rewriting the rest. The canonical template
lives on the device at `/misc/_root_postboot.sh.tpl`.

## 3. Camera topology

- Sensor: **`OV8856`** 8MP RGB (`ov8856_mipi`, "sensor0"), plus a ToF sensor (`ofilm0092`,
  "sensor1").
- Allwinner VIN pipeline: `sunxi_mipi` → `sunxi_csi` → `sunxi_isp` → `sunxi_scaler.0..3`. There are
  **four scalers**, i.e. multiple output streams are *hardware*-possible, exposed as `/dev/video0..2`
  (`vin_video0/1/2`).
- Frame format out of the pipeline: **NV21** (YUV420SP): a `W*H` Y plane followed by an interleaved
  `W*H/2` VU plane; total `W*H*3/2` bytes.
- `ava` opens **`/dev/video2`** for AI navigation while cleaning. `video_monitor` opens
  **`/dev/video0`**. Different device nodes, **same physical sensor**.

## 4. The camera conflict (why we can't just leave the streamer running)

Test: start `video_monitor` (camera held), then start a cleaning job and watch `/proc/*/fd` for who
holds `/dev/video*`.

Observed (times UTC):

- Both handles open simultaneously during cleaning:
  > `video_monitor[13147]: /dev/video0` and `ava[1565]: /dev/video2`
- `video_monitor` then logs dequeue failures:
  > `[ERROR 2026-07-05 16:12:07.950507 log_zm.cpp:6] [VportDequeue] /dev/video0 input Select timeout`
  > `[ERROR 2026-07-05 16:12:07.950862 log_zm.cpp:6] [VideoInputThread] Dequeue error!`
- Shortly after, the **robot reboots** (uptime resets to a few minutes).

Conclusion: concurrent capture on the shared sensor destabilizes the VIN pipeline and the robot
resets. **Source A must be killed before the robot starts cleaning** — which is exactly what
`supervisor.sh` does (it only runs Source A in state `docked`). Streaming during cleaning must
therefore *not* open the camera again — hence Phase 2 taps the existing internal frames instead.

## 5. How Phase 2 taps camera frames (verified)

The first hypothesis — subscribe to an external nanomsg bus `ipc:///tmp/avamsg.socket` — was **wrong
for this firmware** and is recorded here so nobody repeats it:

- `libmessenger.so` contains the string `ipc:///tmp/avamsg.socket`, but the socket is **never bound**:
  `/proc/net/unix` on a running robot shows only `avacmd.socket`, `avaexec.socket`,
  `videomonitor.socket` — no `avamsg.socket`. `libmessenger.so` does not even import `nn_*`.
- `ava` is **monolithic**: one process (`/usr/bin/ava`) dlopens every `node_*.so`. `camera_streamer`
  publishes `ava::Publisher<ava_ai_camera_msg>` **in-process** to `node_camera_ai` (MNN). There is no
  external subscriber, so the bus carrying frames is never exposed.

The frames therefore have to be tapped **inside** `ava`. The clean, interposable boundary:

- `node_camera_streamer.so` is NEEDED-linked against **`libsunxicamera.so`** and calls, across the
  `.so` boundary (so LD_PRELOAD can interpose them), the exported symbols:
  - `sunxi_cam::SunxiCam::OpenCamera(self, a1, fourcc, a3, width, height)` — the 4th/5th int args
    are width/height (verified: `CCameraModeSuxi::OpenCamera` stores them at `this+0x120/0x124` then
    tail-calls SunxiCam::OpenCamera).
  - `sunxi_cam::SunxiCam::GetImageFrame(self, ImageFrame* out)` — called once per captured frame.

`ImageFrame` layout, from disassembling `libsunxicamera.so::GetImageFrame` (it copies fields out of a
72-byte internal buffer):

```c
struct sunxi_cam::ImageFrame {   // 0x28 bytes
    uint64_t f00;   // +0x00  (buf+0x30)
    uint64_t f08;   // +0x08  (buf+0x34)
    uint64_t f10;   // +0x10  (buf+0x38)
    void    *p18;   // +0x18  (buf+0x40)  physical addr?
    void    *data;  // +0x20  (buf+0x00)  <-- NV21 virtual pointer
};
```

The caller `CCameraModeSuxi::GetImageFrame` confirms the semantics: it computes
`size = width*height*3/2` (the disasm literally does `w*h`, `*3`, `/2` — **NV21**) and `memcpy`s from
`ImageFrame+0x20`. So: **hook `GetImageFrame`, read `*(void**)(frame+0x20)`, size `w*h*3/2`.** That is
exactly what `libcamtap.so` does; the width/height come from the `OpenCamera` hook.

The tap runs inside `ava` and must be minimal and defensive — it does a single `memcpy` of the frame
into a tmpfs seqlock buffer (`/tmp/camtap.shm`) and returns. All encoding/network happens in a
separate process (`ava_cam_relay`) so a bug there can never crash navigation.

### On-device test result (the real blocker: the RGB camera does not stream during cleaning)

Verified end to end on the real robot, with the tap instrumented with `calls` / `okframes` / `opens`
counters:

- **The hook works.** At ava startup the tap captured a burst of real frames; the shm header decoded
  exactly: magic `CMPT`, `672x504`, size `0x7C080 = 508032 = 672*504*3/2` — NV21 confirmed, dims and
  layout correct. LD_PRELOAD interposition of the dlopen'd plugin's cross-`.so` call works.
- **During cleaning there is nothing to tap.** In a 55 s cleaning run: `opens` went 1→2 (the RGB camera
  *is* opened for cleaning), `calls` climbed slowly (~0.35/s — `camera_streamer` polls GetImageFrame),
  but `okframes` stayed **flat at the startup value** — i.e. the real `GetImageFrame` returned 0 on
  **every** cleaning call. The RGB camera never enters streaming state; no frame is ever produced for
  us *or* for `ava`.
- **Why:** disassembly of `AvaNodeCameraStreamer::AvaCameraCtrlMsgProcess(ava_msg_ai_camera_switch*)`
  shows the RGB stream is gated by an "AI camera" switch: it sets `this+0xa4 = 3` (on) / `1` (off) from
  the message byte. During autonomous cleaning that switch is **off** — the RGB camera (OV8856) is the
  vendor's *remote-video-monitoring* camera, not the navigation sensor. Obstacle avoidance uses the
  separate structured-light/ToF sensor (`ofilm0092`, sensor1), and `AIObstacleDetectionControlCapability`
  is absent from Valetudo on this model. So `camera_streamer` opens the RGB camera during cleaning but
  keeps it idle (never calls `SunxiCam::start`), and `GetImageFrame` returns 0.
- The earlier "copy destabilizes the ISP" reading was **wrong**: in that run `okframes` was 0 too, so
  our `memcpy` never actually executed — the ISP errors were run-to-run camera noise, not caused by us.

**Force attempt (tried — does not work).** Instrumenting the SunxiCam object during cleaning showed
`state = 3` (software says "streaming") and `ctx != NULL` (context allocated), yet the internal
`dqbuf` returns nothing every time — the sensor produces no frames. So the block is not the software
switch; it is the **hardware**. The injected lib was then made to force streaming: call the real
`SunxiCam::start()` (level 1: only when safe; level 2: `shutdown()` + `start()` repeatedly, even when
`state==3`). Over a cleaning run it forced a re-init 9 times (`forced` counter climbed 1→9) and
`okframes` **never moved** — zero frames. Notably there was also **no reboot and no ISP error** this
time, i.e. the forced `start` doesn't even get the RGB pipeline to run.

**What actually streams during cleaning (ioctl tap).** Interposing `ioctl()` and counting `VIDIOC_DQBUF`
per `/dev/videoN` during cleaning gave the precise picture (note: the platform has **two** ISP pipelines
— `sunxi_isp.0` + `sunxi_isp.1`, two CSI/MIPI, sensors `ofilm0092` and `ov8856_mipi` — so there is *no*
single-ISP contention; the earlier "one ISP" reading was wrong):

- `/dev/video2` = the **RGB** camera (verified: its `VIDIOC_S_FMT` is `672x504 NV21`, sizeimage
  `508032`). It is opened during cleaning but **does not dequeue** — `dqbuf[2]` stays flat. RGB was
  attacked from two angles, both failing:
  - **Raw force** — aggressive `shutdown()`+`start()` on the SunxiCam (8 re-inits, 15 sensor power
    cycles): zero new RGB frames, 278 ISP errors.
  - **Proper path (AI-camera switch)** — the injected lib located the single `AvaNodeCameraStreamer`
    instance in ava's heap by its exported vtable (`_ZTV…AvaNodeCameraStreamer`, object found at a
    stable address) and set the AI-camera switch `this+0xa4 = 3` (exactly what
    `AvaCameraCtrlMsgProcess` does for "on"). `camera_streamer` **reacted** — it tried to bring the RGB
    camera up (15 OV8856 power cycles, 276 ISP errors) — but still produced **zero** frames
    (`dqbuf[2]`/`okframes` flat). Notably `this+0xa4` read back as **2** during cleaning (a dedicated
    "cleaning" camera state), and `camera_streamer` kept re-trying and failing on its own.

  Both confirm the same wall: during cleaning the RGB pipeline is gated off below software control
  (power/clock/firmware policy — the two ISPs are independent, so it is *not* ISP contention). The
  sensor can be powered but delivers no MIPI frames while cleaning, so RGB cannot be made to stream.

- **Manual / remote-control mode is the same.** Entering `HighResolutionManualControlCapability` (with
  or without movement) does **not** turn the RGB camera on either: `video1` (ToF) streams continuously
  (~8–10 fps, even standing still), while `video2` (RGB) stays flat and `isp0` runs a continuous error
  loop — `[VIN_ERR] isp0 frame error, size 0` / `sunxi_isp_reset: ISP frame number is 0` / `8856 pd io`.
  So the RGB pipeline fails whenever the robot is in any *active* mode (cleaning or manual), not just
  cleaning. RGB only works when fully idle/docked (the init burst at camera cold-open, and the
  video_monitor remote-view path used by Phase 1). Conclusion is unchanged: no live RGB view while the
  robot is operating.
- `/dev/video1` = the **ToF / depth sensor** path. It **does** stream during cleaning: `dqbuf[1]` climbed
  0→167 in ~26 s (~8 fps). Its `S_FMT` is `224x1558`, fourcc **`BG12`** (12-bit, `698368` bytes/frame).

### video1 IS viewable — it is an infrared image (Phase 2 reopened)

Grabbing a real `video1` frame (self-`mmap` of the dequeued plane in the `ioctl` hook) and decoding it
showed the `224x1558` frame is **9 infrared sub-frames of `224x173` stacked vertically** (row
autocorrelation gives a clean 173-row period; the first sub-frame is a dark reference). Each sub-frame
is a **recognizable grayscale IR image** of the room from the robot's floor-level view — furniture
legs, a stool, floor, walls — essentially night vision. It is exactly what the robot navigates by, and
it streams the whole time it cleans.

**So a live "watch during cleaning" feed IS achievable — as infrared, not RGB color.** Pipeline:
1. Tap `/dev/video1` in ava (`ioctl`/`VIDIOC_DQBUF` hook + self-mmap the plane) — **built** (this is how
   the frame was captured).
2. Unstack the 173-row bands; take the sharpest sub-frame, or max-project several for more detail.
3. Flat-field (subtract the per-column fixed-pattern stripes) + contrast-stretch to 8-bit grayscale.
4. Rotate 180° (the ToF sensor is mounted rotated), then encode → MJPEG → go2rtc.

**Resolution (this is what shipped).** This is a genuine live view of the robot cleaning — in
**infrared**, not RGB. RGB is unavailable during autonomous cleaning because cleaning spins the LDS
turret, which wedges its ISP (isp0) — see §"Correction" below, which superseded the earlier "firmware
keeps it idle" reading. Either way the turret is spinning while cleaning, so infrared is the only
cleaning-time feed; that limit stands.
What changed vs the pessimistic wording that used to be here:
- The dock view **is** RGB after all — see the RGB-on-dock section below. RGB never streams *while
  cleaning*, but on the dock we drive it ourselves.
- The encoder is **software JPEG**, not CedarX H264. CedarX produced a valid IDR but wouldn't expose
  SPS/PPS and corrupted its own context after one frame — too fragile. A self-contained baseline JPEG
  encoder (color for RGB, grayscale-as-YCbCr for IR) → MJPEG → go2rtc is robust and fully controlled.

So the deliverable is a single go2rtc stream: RGB on the dock, infrared while cleaning.

### Correction (superseded): the blocker is the spinning LDS turret, not an active-mode firmware gate

The reasoning above concluded that the RGB pipeline is "gated off below software control (power/clock/
firmware policy)" whenever the robot is in any active mode. **That conclusion is wrong.** Re-verified
later with `ava` provably dead (all four `ava` watchdogs frozen — an earlier `ava`-respawn confound is
exactly what made this look like an "active-mode" policy), the real cause of the
`[VIN_ERR] isp0 frame error, size 0` flood is the **spinning LDS turret**:

- **The turret, not the mode.** With the turret **parked**, RGB streams cleanly (0 isp0 errors) — even
  while driving (`ros2dreame`'s `W10_NO_TURRET=1` drives with RGB+IR and 0 isp0 errors). With the turret
  **spinning**, it disrupts the OV8856 MIPI (the horizontal-blank timing jitters ~2x; no regulator/clock
  change is logged at the transition, so it is physical — turret-motor EMI / shared-rail droop, not a
  firmware mode switch) and RGB stalls within seconds. So RGB and `/scan` (turret spinning) are
  **mutually exclusive**; RGB-vs-*motion* was never the trade-off. Cleaning happens to spin the turret,
  which is why RGB is unavailable during cleaning.
- **It wedges isp0 persistently.** Once the turret has spun, a plain reopen of `/dev/video2` keeps
  erroring `size 0` even after the turret stops. It does **not** clear by waiting, reopening, a
  proactive stop-before-turret, a VIN unbind (which oopses the driver), the `resetsync` ioctl, or
  persistent streaming.
- **Off-dock un-wedge (solved — no `ava`, no reboot).** Send the MCU **camera-AI-reset** frame
  `0x1d [0x05, 0x00]` with the turret off and the RGB camera **closed**, then reopen `/dev/video2` —
  isp0 recovers and RGB streams clean. Byte0 must be **`0x00`** (reset); `0x01` (what nav sends) does
  **not** clear it. Found by disassembling `node_signal.so`:
  `AvaNodeSignal::AIReset2ComProcess(ava_camera_ai_reset_msg*)` builds `CastComMsg(0x1d, {0x05, byte0}, 2)`
  (siblings: `{0x04,..}` = stereo cam, `{0x01,..}` = ToF).

This supersedes the "manual/remote-control mode is the same" and "gated off below software control"
readings above — both were taken with the turret spinning. The two ISPs are still independent (the ToF
on isp1 is unaffected) and the RGB↔ToF concurrency finding stands — see
[`../phase3-noava/README.md`](../phase3-noava/README.md). This is how `ros2dreame` streams RGB with the
turret parked and recovers a turret-wedged isp0 off-dock.

## 5a. H264 encoder (CedarX `libvencoder.so`)

`ava_cam_relay` encodes the tapped NV21 with the SoC's hardware encoder. `/usr/lib/libvencoder.so` is
the stock CedarX encoder wrapper — NEEDED-linked against `libvenc_codec.so`, `libvenc_base.so`,
`libVE.so`, `libcdc_base.so`, `libMemAdapter.so` — exposing the classic Allwinner `VideoEnc*` C API
(`VideoEncCreate` / `VideoEncInit` / `AllocInputBuffer` / `GetOneAllocInputBuffer` /
`FlushCacheAllocInputBuffer` / `AddOneInputBuffer` / `VideoEncodeOneFrame` / `GetOneBitstreamFrame`).
We `dlopen` it. `VencBaseConfig` offsets used were checked against a disassembly of `VideoEncInit`
(`nDstWidth@0x0c` is compared to `0xEFF`=3839 as a max; `nDstHeight@0x10`; the wrapper fills the
memops/veops pointers at `0x20/0x28/0x30` itself). Input format for NV21 = `VENC_PIXEL_YVU420SP` (1).
The frame plumbing is stable; the H264 rate/GOP *parameter indices* are the standard enum values and
are the one thing worth confirming on-device.

## 5b. Injecting the tap into `ava`

`ava` starts from `/etc/rc.d/ava.sh` (`ava -f /ava/conf/r2104.conf force &`), `/usr/bin/ava` on the
read-only squashfs. There is no `/etc/ld.so.preload` and `/etc` can't be written. So the tap is
injected by **bind-mounting a wrapper over `/usr/bin/ava`**: the wrapper sets
`LD_PRELOAD=/data/camstream/libcamtap.so` and `exec`s a copy of the real `ava`. `ava.sh` then launches
our wrapper. This is opt-in and **restarts `ava`** (the navigation brain) — do it with the robot idle
on the dock; recover with `inject-ava.sh remove` over SSH. Persistence is a marked block in
`_root_postboot.sh` that re-applies the bind mount at boot.

## 6. The config overlay (why `install.sh` bind-mounts `/ava/conf`)

`video_monitor.cfg` (from vacuumstreamer) hardcodes
`recorder_cfg_path = /ava/conf/video_monitor/recorder.cfg`. On the W10 there is **no**
`/ava/conf/video_monitor`, and `/ava` is read-only squashfs — we cannot create it in place.

Fix: `install.sh` builds `/data/camstream/ava_conf_ovl` = a full copy of `/ava/conf` **plus** a
`video_monitor/` subdir, then `supervisor.sh` `mount --bind`s it over `/ava/conf`. The overlay is a
superset of the original, so `ava` sees everything it did before, plus the video_monitor configs.
`recorder.cfg` uses `864x480 @15fps YVU420SP`, `video_use_wm = 0` (the missing-watermark error
`/rom/etc/res/wm_540p_0.bmp` is non-fatal). `/mnt/private` is bind-mounted the same way to satisfy
`video_monitor`'s Agora init reading the device cert/ULI.

## 7. Source A internals (vacuumstreamer)

- `video_monitor` (proprietary Dreame binary) opens the camera and hands H264 to the Agora RTC SDK.
- `vacuumstreamer.so` is `LD_PRELOAD`ed over the Agora SDK and hooks `setQueueH264Buffer`, diverting
  the already-encoded H264 to **TCP `127.0.0.1:6969`** instead of the cloud.
- `video_monitor` has a **singleton guard** (`ps | grep video_monitor`): if the launching process
  still has `video_monitor` in its argv, it aborts with "video_monitor is Running, Please Check".
  Hence `run_vm.sh` ends with `exec ./video_monitor` (the launcher shell is replaced, leaving exactly
  one match). A stale `/tmp/videomonitor.socket` also makes it exit immediately, so we `rm -f` it
  first.
- vacuumstreamer is built for the L10s Ultra (`r2228`) but its `video_monitor` and `libc.so.6_r2228`
  are also glibc `2.23`, so it runs on the W10.

## 8. Bonus: MCU protocol + the docking diagnosis (methodology reference)

Not needed for streaming, but this is how the robot's lower half was decoded — useful if you extend
this work. The wheel/dock/IMU MCU speaks a binary protocol on **`/dev/ttyS4`**, reverse-engineered
using [alufers/dreame_mcu_protocol](https://github.com/alufers/dreame_mcu_protocol) as a base:

- Framing: `<` (0x3c) … `>` (0x3e), escape `?` (0x3f); body `[len][type][payload][CRC16-Modbus]`.
- From MCU: `0x00` Triggers (dock IR `ir_dock_lf/lmf/rmf/rf`, `left/right_wheel_overcurrent`,
  `left/right_vel_error`, `charge_error`, `wheel_floating`), `0x01` Status@20ms (W10: 30B; leftVel,
  rightVel, yaw), `0x02` Status@10ms (IMU), `0x03` Status@100ms (offsets 4/6 = left/right wheel
  current), `0x2b` BatteryStatus.
- To MCU: `0x00` MotorCtrl (`<Bff` = flag, linear_velocity, rotational_velocity).

Capture method: an old-glibc static `strace` (Ubuntu 16.04 `arm64` `strace 4.11`, glibc `2.17`)
attached to `ava` filtering `/dev/ttyS4`, decoded with an adapted `mcu_packets.py`. This produced a
**definitive hardware diagnosis** of a docking failure: during final seating the robot commanded
back-and-turn (`linear=-100`, `rotational=-0.9`) while `rightVel` stayed 0 and `right_vel_error` held
~15s, with wheel current spiking erratically (up to ~5028) at near-zero motion — a stalled/failing
**right wheel motor**. Replacing the motor fixed docking. The lesson for this repo: the robot's own
MCU telemetry is the ground truth, and code/behavior patches (we tried several on
`libaether_route_nodes.so`) could not fix a hardware fault.

---

## 9. RGB on the dock + crash-safety (the final working design)

The pessimistic notes above are about RGB *during cleaning*. On the **dock** it does work:

- **The camera works; darkness fooled us.** The first captured dock frame was near-black
  (`Y mean ≈ 1`) — but that was a dark room. With a light on, the same tap gives a clean full-color
  image (`Y` spans 2..254). So the RGB path is fine; it just needs light.
- **`camera_streamer` is idle on the dock,** so we drive the camera ourselves from a thread in the
  shim: `SunxiCam::OpenCamera` once, then loop `GetImageFrame` → publish → `ReturnImageFrame`. The
  **`ReturnImageFrame` (qbuf) is the crux** — without re-queuing, the 3-buffer pool starves after ~3
  frames (which is why earlier attempts saw only a brief burst). With it, it streams continuously at
  ~14 fps. Setting the AI-camera switch (`this+0xa4=3`) alone does **not** start a stream.
- **Crash-safety.** `ava`'s `camera_streamer` and our drive thread share the *same* `SunxiCam` object;
  letting both manage its lifecycle deadlocked `ava` mid-clean (the robot "hangs"; `ava` is killed by
  the watchdog — no segfault in `dmesg`). Fix: one mutex serializes every real `SunxiCam` call (ours +
  `ava`'s, which all pass through our interposed `OpenCamera`/`GetImageFrame`/`CloseCamera` hooks), and
  an explicit ownership handoff — when `ava` opens the camera (cleaning) the hook closes the drive
  thread's session under the lock and takes over; the drive thread yields whenever `ava` is active
  (recent hook calls) or the ToF stream is live. Verified stable across many dock↔clean cycles.

## 10. go2rtc chokes on grayscale JPEG

When the feed switches to the infrared (grayscale) image, go2rtc **panics and dies**:

> `panic: interface conversion: image.Image is *image.Gray, not *image.YCbCr`
> `github.com/AlexxIT/go2rtc/pkg/mjpeg.Transcode`

Its MJPEG transcoder hard-casts decoded frames to `*image.YCbCr`. Fix: encode the IR frame as a
3-component **YCbCr** JPEG with neutral chroma (`U=V=128`) — visually identical grayscale, but go2rtc
is happy. Both feeds are therefore YCbCr JPEGs.

## Tooling used

- Local `clang` cross-compile to `aarch64-linux-gnu` (no aarch64 gcc needed) — same as vacuumstreamer.
- `strace_arm64` = Ubuntu xenial `strace_4.11-1ubuntu3_arm64` (runs on the robot's glibc 2.23).
- Python decoders (capstone, pyelftools, cxxfilt) for the ELF/behavior-tree and MCU stream analysis.
- Transfers over `ssh 'cat > file'` / `tar | ssh 'tar -x'` — the BusyBox `sshd` has **no
  `sftp-server`**, so `scp -O` fails with `/usr/libexec/sftp-server: not found`.
