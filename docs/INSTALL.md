# Install walkthrough

## 0. Prerequisites

- A **rooted** Dreame W10 (`r2104`) reachable over SSH: `ssh root@<robot-ip>` works.
- On your workstation: `make`, `ssh`, `tar`, `curl`, and **`docker`** (used to cross-compile the two
  aarch64 artifacts reproducibly; no host toolchain needed). `ffmpeg` only if you want `make view`.
- The robot already has a `go2rtc` binary (from the dustbuilder/vacuumstreamer setup, at
  `/data/vacuumstreamer/go2rtc`). If not, `make build` downloads one and it is uploaded too.

> Why no `scp`: the robot's BusyBox `sshd` ships no `sftp-server`, so `scp -O` fails with
> `/usr/libexec/sftp-server: not found`. Everything here uses `tar | ssh 'tar -x'` instead.

## 1. Configure

```sh
cp config.mk.example config.mk
$EDITOR config.mk          # set ROBOT=root@<robot-ip>
```

`ROBOT` is the only value you must set. `REMOTE_DIR` (install path under `/data`) and `GO2RTC_VERSION`
(only used if the robot has no `go2rtc` yet) have working defaults.

## 2. Build

```sh
make build
```

Downloads `go2rtc` (arm64) and cross-compiles `libcamtap.so` + `ava_cam_relay` for aarch64 in a pinned
Docker image, then stages everything into `build/stage/`.

## 3. Upload

```sh
make upload
```

Creates `$(REMOTE_DIR)` (default `/data/camstream`) on the robot and streams the staged tree over
`tar`+`ssh`.

## 4. Install — inject the shim (restarts ava)

```sh
make install
```

This runs `inject-ava.sh install` on the robot: it snapshots the stock `ava`, bind-mounts a wrapper
over `/usr/bin/ava` that `LD_PRELOAD`s `libcamtap.so`, writes `camtap.env` (`CAMTAP_FORCE=5` +
`CAMTAP_IR=1`), adds the boot-persistence block, **restarts `ava`**, and starts the feed (relay +
go2rtc).

> `ava` is the navigation brain — run this with the robot **idle on the dock**. If anything is wrong,
> `make uninstall` (or `inject-ava.sh remove` over SSH) restores stock `ava`.

## 5. Watch

```sh
make watch          # prints the URLs
```

- Web UI : `http://<robot-ip>:1984/`  (stream `camera`)
- RTSP   : `rtsp://<robot-ip>:8554/camera`  (VLC → Open Network Stream)
- WebRTC : `http://<robot-ip>:1984/webrtc.html?src=camera`
- Snap   : `http://<robot-ip>:1984/api/frame.jpeg?src=camera`

On the dock you get **color RGB**; start a cleaning and it auto-switches to **infrared**. `make view`
grabs a single frame to `build/frame.jpg` to confirm.

## Control & teardown

```sh
make start        # (re)start the relay + go2rtc
make stop         # stop them
make status       # is the shim active in ava?
make uninstall    # remove the shim, restart stock ava
```

## Troubleshooting

- **Robot hangs during cleaning** — an old build where the drive thread raced `ava` over the camera.
  The current shim serializes all `SunxiCam` access with a mutex; rebuild + redeploy `libcamtap.so`.
- **go2rtc dies when cleaning starts** — an old relay that emitted a 1-component grayscale JPEG (go2rtc
  panics on that). The current relay encodes IR as YCbCr; rebuild + redeploy `ava_cam_relay`.
- **No RGB on the dock** — with the room dark the image is nearly black (it's a real camera, not IR);
  turn a light on. Check `make status` shows the tap active.
- **Blank feed while cleaning** — the ToF stream starts a few seconds after undocking; give it a moment.

## History

An earlier approach used the vendor `video_monitor` (via [tihmstar/vacuumstreamer]) for the dock RGB
view and a `supervisor.sh` to switch sources. It was dropped: `video_monitor` idles forever on a
de-clouded robot (it waits for a cloud "start" command it never gets), so the dock RGB now comes from
the in-`ava` drive thread instead. Those scripts have been removed from the tree; the full
investigation is in [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md).

[tihmstar/vacuumstreamer]: https://github.com/tihmstar/vacuumstreamer
