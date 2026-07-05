# Architecture

## Goal

One camera feed from a rooted Dreame W10, viewable **on the dock** and **during cleaning**, exposed
as RTSP/WebRTC via go2rtc, that never destabilizes the robot.

## The hard constraint

The W10 has **one** physical camera (an `OV8856`). The robot needs it for AI obstacle avoidance while
cleaning — `ava` opens it as `/dev/video2`. The stock streaming path (`video_monitor`) opens it as
`/dev/video0`. Both map to the **same sensor**. When both are open at once during cleaning, the robot
**reboots** (observed; see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md#the-camera-conflict)).

So we cannot "just run `video_monitor` all the time". We need two mutually-exclusive sources and a
switch.

## Components

```
 ┌──────────────────────────────────────────────────────────────────────┐
 │ robot (rooted Dreame W10, /data is the only writable fs)              │
 │                                                                        │
 │  Valetudo REST  ◄─────poll──── supervisor.sh                          │
 │  :80 /api/v2                     │  decides A vs B from robot state    │
 │                                  │                                     │
 │            ┌──────── docked ─────┴───── cleaning/moving ───────┐       │
 │            ▼                                                    ▼       │
 │   Source A: video_monitor                        Source B: libcamtap + ava_cam_relay
 │   + vacuumstreamer.so (LD_PRELOAD)               (Phase 2, in-ava tap)  │
 │   opens /dev/video0                              LD_PRELOAD in ava taps │
 │   Agora hook diverts H264 ──┐                    SunxiCam::GetImageFrame │
 │                             │                    NV21─►shm─►H264 ──┐    │
 │                             ▼                                   ▼       │
 │                       127.0.0.1:6969  ◄────────────────────────┘       │
 │                             │  (single H264 producer socket)           │
 │                             ▼                                          │
 │                          go2rtc  ──► :8554 RTSP  :8555 WebRTC  :1984 web│
 └──────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼  LAN
                    browser / VLC / Home Assistant
```

- **`supervisor.sh`** — the only always-on decision maker. Polls Valetudo `robot/state/attributes`
  every few seconds, reads `StatusStateAttribute.value`, and keeps exactly one source alive.
- **Source A (`video_monitor` + `vacuumstreamer.so`)** — from
  [tihmstar/vacuumstreamer](https://github.com/tihmstar/vacuumstreamer). Opens the camera directly and
  the LD_PRELOAD shim reroutes the Agora H264 buffer to TCP `:6969`. Used **only on the dock**.
- **Source B (`libcamtap.so` + `ava_cam_relay`, Phase 2)** — a tiny LD_PRELOAD tap inside `ava`
  interposes `sunxi_cam::SunxiCam::GetImageFrame` and copies each NV21 frame to a tmpfs seqlock
  buffer; a separate process encodes it to H264 (CedarX) and serves `:6969`. Used **when off the
  dock**. No second camera open ⇒ no reboot. Opt-in (restarts `ava`). See
  [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md#5-how-phase-2-taps-camera-frames-verified).
- **go2rtc** — single consumer of `:6969`, single point of external access. Stays up across switches.

## The switch (state → source)

`supervisor.sh` maps Valetudo state to a source:

| Valetudo `StatusStateAttribute.value` | Source | Camera opened by |
|---------------------------------------|--------|------------------|
| `docked`                              | **A**  | video_monitor    |
| `cleaning`, `returning`, `moving`, `paused`, `idle`, `error`, `manual_control` | **B** (or none if relay absent) | ava only |
| (API unreachable)                     | hold current — don't thrash | — |

Only `docked` is treated as camera-safe for Source A, because in every other state the robot may be
off the dock with `ava` about to (or already) using the camera. On each transition the supervisor
stops the old source, waits ~1s for the device to be released, then starts the new one.

Without Phase 2 installed, "Source B" is simply "no stream" — the feed pauses while cleaning and
resumes automatically on docking. That is the safe default.

## Why go2rtc always sees one socket

Both sources publish raw H264 to `127.0.0.1:6969`, and go2rtc's only stream is `tcp://127.0.0.1:6969`.
go2rtc reconnects on its own when the producer changes, so switching A↔B needs no go2rtc restart and
no client-side URL change. Viewers keep the same `rtsp://…:8554/camera` / web URL throughout.

## On-disk layout on the robot (`/data/camstream`)

```
video_monitor              Source A binary (proprietary, from vacuumstreamer)
vacuumstreamer.so          Source A Agora hook
ava_cam_relay              Source B binary (Phase 2, optional)
go2rtc                     streaming server (arm64)
go2rtc.yaml                go2rtc config (:6969 -> rtsp/webrtc/web)
supervisor.sh              the switch (main loop + --stop/--status)
run_vm.sh / run_go2rtc.sh  exec-launchers
install.sh                 on-device setup/teardown
ava_conf_video_monitor/    video_monitor's configs (from vacuumstreamer)
ava_conf_ovl/              full copy of /ava/conf + video_monitor/ (bind-mounted over /ava/conf)
mnt_private_ovl/           copy of this robot's /mnt/private (bind-mounted over /mnt/private)
```

The two `*_ovl` dirs exist because `video_monitor.cfg` hardcodes `/ava/conf/video_monitor/...` and
`/ava` is a read-only squashfs. They are assembled on-device by `install.sh` from the robot's own
files, so nothing device-specific ever lives in the repo. See
[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md#the-config-overlay).

## Persistence

`install.sh` appends a marked block to `/data/_root_postboot.sh` (the root boot hook invoked from
`/etc/rc.sysinit`) that relaunches `supervisor.sh` after every reboot. `make uninstall` removes the
block by marker. It never rewrites the rest of `_root_postboot.sh` (which also launches Valetudo).
