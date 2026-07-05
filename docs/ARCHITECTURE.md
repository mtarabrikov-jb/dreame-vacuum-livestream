# Architecture

## Goal

One camera feed from a rooted Dreame W10, served through the robot's own go2rtc as RTSP / WebRTC /
MSE, whose content **auto-switches with the robot's state** and **never destabilizes navigation**:

- **on the dock** — full-color RGB (the room from floor level)
- **while cleaning** — grayscale infrared (the ToF/obstacle sensor's view)

## Why it's built this way

The W10 will not stream its RGB camera during cleaning — the firmware keeps the OV8856 idle then and
uses the separate ToF sensor for obstacle avoidance (proven in
[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md#on-device-test-result-the-real-blocker-the-rgb-camera-does-not-stream-during-cleaning)).
The vendor's `video_monitor` streamer can't help either: on a de-clouded (Valetudo) robot it idles
forever waiting for an Agora "start" command that never arrives. So both feeds are produced from
**inside `ava`** instead:

- On the dock, `ava`'s own `camera_streamer` isn't using the RGB camera, so we drive it ourselves.
- While cleaning, we passively tap the ToF frames `ava` is already capturing — no second camera open.

## Components

```
 ┌───────────────────────────────────────────────────────────────────────────┐
 │ robot (rooted Dreame W10, /data is the only writable fs)                   │
 │                                                                             │
 │  ava (navigation brain, LD_PRELOAD = libcamtap.so)                         │
 │   ├─ dock : drive thread owns SunxiCam (OpenCamera/GetImageFrame/Return)   │
 │   │        → NV21 672x504 ─────────────┐                                   │
 │   └─ clean: ioctl(VIDIOC_DQBUF) tap of /dev/video1 (ToF)                   │
 │            → raw 224x1558 ─────────────┤                                   │
 │        (one g_cam_lock mutex + ownership handoff = no clash with ava)      │
 │                                        ▼                                    │
 │                                 /tmp/camtap.shm  (tmpfs, seqlock)          │
 │                                        │                                    │
 │                                 ava_cam_relay  (separate process)          │
 │                                   fmt 0  → color JPEG (NV21→YCbCr)         │
 │                                   fmt 100→ IR JPEG (unstack+flatfield,     │
 │                                            YCbCr neutral chroma, rot 180)  │
 │                                        │ MJPEG                              │
 │                                        ▼                                    │
 │                                 127.0.0.1:8090  (loopback)                 │
 │                                        │                                    │
 │                                 go2rtc  ──► :8554 RTSP  :8555 WebRTC  :1984 │
 └───────────────────────────────────────────────────────────────────────────┘
                                          │  LAN
                                          ▼
                           browser / VLC / Home Assistant   (stream: camera)
```

- **`libcamtap.so`** — LD_PRELOAD'd into `ava`. Interposes the exported `sunxi_cam::SunxiCam` symbols
  (`OpenCamera` / `GetImageFrame` / `CloseCamera`) and the libc `ioctl`. A background thread drives the
  RGB camera on the dock; the `ioctl` hook copies ToF frames while cleaning. All writes go to the
  tmpfs seqlock buffer `/tmp/camtap.shm`.
- **`ava_cam_relay`** — out-of-process. Reads whichever frame is in the buffer, encodes it to JPEG
  with a self-contained baseline encoder (color 4:2:0 for RGB; grayscale-as-YCbCr for IR), and serves
  MJPEG on `127.0.0.1:8090`. Running outside `ava` means an encoder bug can't crash navigation.
- **go2rtc** — the robot's existing `/data/vacuumstreamer/go2rtc`. Consumes the loopback MJPEG as the
  single stream `camera` and re-serves RTSP / WebRTC / MSE / snapshots. One URL throughout.

## The switch (who owns the camera)

There is no external supervisor — the switch is intrinsic:

- The **relay auto-picks** by the shm frame's `format` field: `0` = NV21 (color RGB), `100` = raw ToF
  (infrared). Whatever the shim last wrote is what go2rtc serves.
- The **shim's drive thread yields** the RGB camera the instant `ava` needs it. `cs_busy()` is true
  when `camera_streamer`'s hooks fired recently **or** the ToF stream is live (cleaning) — in either
  case the drive thread closes the RGB camera and stops. When the robot re-docks and `ava` goes quiet,
  it re-acquires and RGB resumes.

## Crash-safety (the key invariant)

`ava`'s `camera_streamer` and our drive thread use the **same** `SunxiCam` object. Letting both own its
lifecycle deadlocked `ava` (the robot hung mid-clean). The fix, in `libcamtap.c`:

- **One mutex `g_cam_lock`** wraps *every* real `SunxiCam` call — from the drive thread and from
  `ava`, whose `OpenCamera`/`GetImageFrame`/`CloseCamera` all pass through our interposed hooks. No two
  threads are ever inside a `SunxiCam` function at once.
- **Explicit ownership** (`g_owner` = none / drive / streamer). When `ava` calls `OpenCamera`
  (cleaning starting), the hook closes the drive thread's session under the lock and takes over;
  `CloseCamera` returns ownership; the drive thread only acquires when `g_owner == none` and `ava` is
  idle.

Result: `ava` stays up across dock→clean→dock transitions (verified over multiple cycles).

## On-disk layout on the robot (`/data/camstream`)

```
libcamtap.so     the in-ava tap/drive shim (LD_PRELOAD)
ava_cam_relay    ToF/RGB frame -> JPEG -> MJPEG :8090
go2rtc_ir.yaml   go2rtc config: consume :8090, serve stream "camera"
run_ir.sh        start/stop the relay + go2rtc
inject-ava.sh    inject/remove the shim (bind-mount wrapper over /usr/bin/ava), + boot persistence
camtap.env       CAMTAP_FORCE=5 (RGB drive) + CAMTAP_IR=1 (ToF tap)
ava_real/ava     snapshot of the stock ava the wrapper exec's with LD_PRELOAD
```

## Injection & persistence

`/etc` is a read-only squashfs with no `/etc/ld.so.preload`, so `inject-ava.sh` bind-mounts a tiny
wrapper over `/usr/bin/ava` that sets `LD_PRELOAD=…/libcamtap.so` and `exec`s a copy of the real `ava`.
It adds a marked block to `/data/_root_postboot.sh` (the root boot hook) that re-applies the bind mount
and starts the feed after every reboot. `make uninstall` removes the block and restores stock
`ava`. Injecting **restarts `ava`** — do it with the robot idle on the dock.
