# Install walkthrough

## 0. Prerequisites

- A **rooted** Dreame W10 (`r2104`) reachable over SSH: `ssh root@<robot-ip>` works.
- On your workstation: `make`, `ssh`, `tar`, `curl`; `clang` only if you want to build Phase 2;
  `ffmpeg` only if you want `make view`.
- A **built** [tihmstar/vacuumstreamer](https://github.com/tihmstar/vacuumstreamer) checkout providing
  `dist/usr/bin/video_monitor`, `vacuumstreamer.so`, and `dist/ava/conf/video_monitor/`.

> Why no `scp`: the robot's BusyBox `sshd` ships no `sftp-server`, so `scp -O` fails with
> `/usr/libexec/sftp-server: not found`. Everything here uses `tar | ssh 'tar -x'` instead.

## 1. Configure

```sh
cp config.mk.example config.mk
$EDITOR config.mk
```

Set at least `ROBOT=root@<robot-ip>` and `VACUUMSTREAMER_DIR=/path/to/your/vacuumstreamer`.

## 2. Build

```sh
make build
```

Downloads `go2rtc` (arm64), cross-compiles Phase 2 if `clang` is present (skipped otherwise), and
stages everything into `build/stage/`.

## 3. Upload

```sh
make upload
```

Creates `$(REMOTE_DIR)` on the robot and streams the staged tree over `tar`+`ssh`.

## 4. Install (on-device setup)

```sh
make install
```

This runs `install.sh` on the robot: assembles the `/ava/conf` and `/mnt/private` overlays from the
robot's own files, and adds a marked boot-hook block to `/data/_root_postboot.sh`.

## 5. Start

```sh
make start
make status
```

`status` shows the robot state, which source is live, and the URLs. Since the robot is on the dock,
you should see **Source A up** and go2rtc listening.

Open **`http://<robot-ip>:1984`** (go2rtc web UI) or **`rtsp://<robot-ip>:8554/camera`** in VLC.
`make view` grabs a single frame to `build/frame.jpg` to confirm the feed.

## 6. Verify the cleaning behavior

Start a cleaning job (app/Valetudo). Within a few seconds `make status` should flip to:
`robot state: cleaning`, `source A: down`. With Phase 2 not yet finished, the stream pauses during
cleaning and resumes automatically when the robot re-docks. The point: **the robot must not reboot**.
If you built and installed a working Phase 2 `ava_cam_relay`, `status` shows `source B: UP` instead.

## Teardown

```sh
make stop        # stop processes, keep files
make uninstall   # stop + unmount overlays + remove the boot-hook block
```

Files stay in `$(REMOTE_DIR)`; `ssh root@<robot-ip> 'rm -rf /data/camstream'` removes them fully.

## Troubleshooting

- **`video_monitor is Running, Please Check`** — a leftover `video_monitor` process, or a launcher
  whose argv contains `video_monitor`. `make stop` clears it; the shipped `run_vm.sh` uses `exec` to
  avoid the false positive.
- **go2rtc shows no stream** — check `make logs`. Source A needs `/tmp/videomonitor.socket` absent
  (the launcher removes it) and the `/ava/conf` overlay mounted (`grep /ava/conf /proc/mounts` on the
  robot).
- **Robot rebooted** — a source opened the camera during cleaning. Confirm `supervisor.sh` is the one
  starting sources (don't run `video_monitor` by hand) and that state polling reaches Valetudo
  (`curl -s http://127.0.0.1/api/v2/robot/state/attributes` on the robot).
