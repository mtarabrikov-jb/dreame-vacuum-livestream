# Reverse-engineering notes (Dreame W10 / `r2104`)

Everything below was determined on a rooted **Dreame Bot W10**, product code **`r2104`**. Other
Dreame models share the software stack (`ava`, CedarX, Valetudo-on-top) but differ in sensor,
product code, and struct layouts. Treat model-specific numbers as W10-only unless you re-verify.

Placeholders: `<robot-ip>` = your robot's LAN address; `root@<robot-ip>` = its SSH login.

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

## 5. `ava` internal IPC bus (Phase 2 target)

- Transport: **nanomsg** (`libnanomsg.so.5`) over `ipc:///tmp/avamsg.socket`, wrapped by
  `libmessenger.so`.
- Messages are "raknetmessage"s: a type discriminator readable via `getMsgType`, and a
  **CRC16-Modbus** integrity field (same CRC the MCU protocol uses; init `0xFFFF`, poly `0xA001`).
- During cleaning, `camera_streamer` publishes the AI frame as **`ava_ai_camera_msg`** (NV21) to the
  AI node on this bus. That is the frame Phase 2 subscribes to — passively, without opening the
  camera.
- Still **TODO** for Phase 2: the numeric message type id, and the exact payload header offsets
  (width/height/stride/format/timestamp) before the NV21 bytes. `ava_cam_relay --dump` on the robot
  during cleaning prints message lengths/types to pin these down.

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

## Tooling used

- Local `clang` cross-compile to `aarch64-linux-gnu` (no aarch64 gcc needed) — same as vacuumstreamer.
- `strace_arm64` = Ubuntu xenial `strace_4.11-1ubuntu3_arm64` (runs on the robot's glibc 2.23).
- Python decoders (capstone, pyelftools, cxxfilt) for the ELF/behavior-tree and MCU stream analysis.
- Transfers over `ssh 'cat > file'` / `tar | ssh 'tar -x'` — the BusyBox `sshd` has **no
  `sftp-server`**, so `scp -O` fails with `/usr/libexec/sftp-server: not found`.
