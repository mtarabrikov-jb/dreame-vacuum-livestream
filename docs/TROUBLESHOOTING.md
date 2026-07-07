# Troubleshooting

## Camera dead — no frames, ISP `size 0` reset loop (`VIN_ERR` flood)

### Symptom

- The livestream freezes: go2rtc serves only the last frame from the moment it broke (a still image, not black), or nothing.
- The tap stops publishing frames: the `seq` / `frames` counters in the `/tmp/camtap.shm` header stop advancing (see the liveness check below). **Do not use the file's mtime** — the tap writes the frame through an `mmap`, which does not update mtime, so mtime stays at the file's creation time whether or not frames are flowing.
- `dmesg` floods with, continuously:
  ```
  [VIN_ERR]isp0 frame error, size 0 1203, hblank max 1271 min 79x!!
  [VIN]sunxi_isp_reset:isp0 reset!!!,ISP frame number is 0
  [VIN_ERR]****************8856 pd io  128 ,frame id 1
  [ov8856_mipi]PWR_OFF! ... PWR_ON! ... r2104 no otp info
  ```
- `ava` pegs one core (~60% CPU, load ~5-6) — its ToF/vision threads (`TofThread`, `TofMapAcc`, `SlamAsync`) busy-loop retrying the dead camera. This is worst during cleaning (when `ava` drives the sensor hardest) and eases when docked-idle, but the `VIN_ERR` flood continues regardless.

Both feeds die together. Per [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) §3, the dock RGB feed (`/dev/video0`) and the cleaning IR feed (`/dev/video2`) are **the same physical OV8856 sensor** on different scaler outputs, so a single sensor/link fault takes out RGB and IR at once.

### Root cause (observed incident, 2026-07-07)

**Hardware fault in the OV8856 MIPI-CSI high-speed data path — the camera flex (FPC) cable.** The failure began mid-cleaning (camera froze at 07:55 UTC, i.e. `/tmp/camtap.shm` mtime stuck there); vibration/movement during cleaning pushed a marginal connection over the edge.

Why the flex cable specifically: the OV8856 FPC carries both a slow I2C control bus and the fast MIPI D-PHY lanes (clock + data differential pairs). The decisive split:

- **I2C is intact** — the sensor answers on the bus, chip-ID reads back, the module EEPROM responds.
- **MIPI carries no data** — every frame is `size 0`, so the ISP receives line-sync but zero pixel payload, resets, power-cycles the sensor, and loops.

That combination (control bus alive, high-speed data dead) is the signature of broken/marginal MIPI differential pairs while the robust low-speed I2C survives — i.e. a cracked or lifted FPC cable/connector, not a total disconnect, not power, not software.

### Diagnostic procedure (reproduce the diagnosis)

Run over SSH (`root@<robot-ip>`). This is what pins it to the cable rather than software:

1. **Is the ISP erroring right now?** Compare the last `VIN_ERR` timestamp to uptime — equal means live:
   ```sh
   dmesg | grep -c VIN_ERR ; dmesg | tail -3 ; cut -d. -f1 /proc/uptime
   ```

2. **Are frames flowing right now?** Read the tap's seqlock header twice, a few seconds apart. The `seq` field (offset 4) bumps `+2` per published frame and `frames` (offset 24) counts them; if either advances, live frames are reaching the tap. **Do not use mtime** — the tap writes via `mmap`, so mtime never moves per frame. On the dock the RGB feed only runs while a consumer is connected, so hold a viewer (or grab a go2rtc frame) while sampling:
   ```sh
   # header: u32 magic, u32 seq, u32 width, u32 height (little-endian)
   dd if=/tmp/camtap.shm bs=32 count=1 2>/dev/null | od -An -tx4 ; sleep 4
   dd if=/tmp/camtap.shm bs=32 count=1 2>/dev/null | od -An -tx4
   # seq advancing (e.g. ~+120/4s = ~15fps) = camera live; frozen = no frames
   ```

3. **Is the sensor alive on I2C?** OV8856 sits at bus 3, address 0x36. `UU` = claimed by the driver; read the chip-ID high byte (16-bit register, so use `i2ctransfer`):
   ```sh
   i2cdetect -y -r 3                                   # expect UU at 0x36, EEPROM at 0x50
   i2ctransfer -f -y 3 w2@0x36 0x30 0x0b r1            # expect 0x88 (OV8856 chip-ID high byte)
   ```
   `0x88` reading back = the sensor is powered and answering on I2C. (Register 0x300C reads `0x5a` while streaming rather than the datasheet `0x56`; the stable `0x88` on 0x300B is the reliable liveness check.)

4. **Does it survive a restart / cold boot?** Restart `ava` (`/etc/rc.d/ava.sh`), then reboot. Re-check step 1 each time.

Interpretation:

| I2C chip-ID (step 3) | MIPI frames (step 1) | Meaning | Action |
|---|---|---|---|
| reads `0x88` | `size 0` flood | **MIPI data lanes dead, control bus alive** | reseat / replace the camera FPC cable |
| no ACK / NAK on 0x36 | `size 0` flood | sensor not responding at all — full disconnect or power | check FPC seating + sensor power rail, then module |
| reads `0x88` | clean, no `VIN_ERR` | camera healthy | look elsewhere (relay/go2rtc — see below) |

If the camera is healthy but the stream is still down, the fault is downstream: restart just the relay + go2rtc (does **not** touch `ava`, safe during cleaning):
```sh
REMOTE_DIR=/data/camstream sh /data/camstream/run_ir.sh
```
If the `seq` counter (step 2) still does not advance after that, the source (ava/sensor) is the problem, not the relay.

### What was ruled out (2026-07-07)

- **Software / our stack.** Not fixed by restarting `ava` (full V4L2 re-open + sensor re-init) nor by a cold reboot (kernel VIN/ISP/MIPI-PHY re-init from scratch — `size 0` reproduced from the first boot-time stream attempt). The tap (`libcamtap.so`) and relay (`ava_cam_relay` + go2rtc) were confirmed healthy by restarting them independently.
- **Thermal.** CPU/GPU/DDR ~66 C — no overheat.
- **Unrelated to the MCU/LDS work.** The robot had rebooted since; only `libcamtap.so` was loaded in `ava` (no `avatap.so`), and that work is on ttyS4/ttyS3 (a separate bus) with nothing on the MIPI/ISP path.

### The fix

Physical only. Power off, open the top cover, and **reseat the OV8856 camera FPC cable** at both ends (camera module and mainboard); inspect it for a crease/crack. Re-test with the diagnostic above — `dmesg | grep -c VIN_ERR` should stop growing and the `seq` counter (step 2) should advance during a stream. If reseating does not fix it, replace the camera module (or, rarely, the mainboard MIPI connector).

### Resolution (2026-07-07)

Reseating the FPC cable (plus a reboot) **fixed it** — which confirms the diagnosis (marginal MIPI contact, not the sensor or software). Recovery signature: a short burst of `VIN_ERR` during boot-time sensor init (~first 30 s), then the flood stops and the stream runs clean — `seq` advancing at ~15 fps (`+~120` per 4 s), buffer content changing, `ava` back to normal CPU. Contrast the failure, where `VIN_ERR` floods continuously for as long as the camera is open. Because the root cause is a marginal contact, it can recur under cleaning vibration.

### Optional mitigation until repaired

Until the cable is fixed, every cleaning makes `ava` burn a core retrying the dead sensor (camera-based obstacle avoidance is already non-functional — bumper, cliff and LDS still protect the robot). To stop the futile retries you can disable the camera/ToF node in `/ava/conf/r2104.conf` (config overlay). This is optional and only cosmetic (CPU/heat); it does not restore the camera.

### Reference (this robot, `r2104`)

- Sensors on I2C bus 3: `ov8856_mipi` @ 0x36 (RGB, "sensor0"), `ofilm0092` (ToF, "sensor1"), module EEPROM @ 0x50.
- OV8856 chip-ID: `0x300B = 0x88`, `0x300C = 0x56` (datasheet).
- VIN pipeline: `sunxi_mipi -> sunxi_csi -> sunxi_isp -> sunxi_scaler.0..3`; nodes `/dev/video0..2`. `ava` uses `/dev/video2` (cleaning), `video_monitor` uses `/dev/video0` (dock) — same physical OV8856.
- Feed switch: dock = RGB, cleaning = grayscale IR (see README) — both fail together on an OV8856/link fault.

## No video in Home Assistant over RTSP (the stream is MJPEG, not H264)

### Symptom

`rtsp://<robot-ip>:8554/camera` shows nothing in Home Assistant, while the exact same stream plays fine in the go2rtc web UI (`http://<robot-ip>:1984/`), VLC, or `ffplay`.

### Cause

The `camera` stream is **MJPEG only**. `ava_cam_relay` serves MJPEG on `127.0.0.1:8090` (YCbCr JPEG for both feeds — see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) §10), go2rtc re-serves it; there is **no H264 producer**, and there is **no `ffmpeg` on the robot** to transcode. Confirm:

```sh
wget -qO- "http://127.0.0.1:1984/api/streams?src=camera"   # -> codec_name: "mjpeg", format_name: "mpjpeg"
command -v ffmpeg || echo "no ffmpeg on robot"
```

Home Assistant's `stream` component (RTSP, recording, HLS) decodes **H264/H265 only** and does not handle MJPEG-over-RTSP, so HA pulls the stream but shows no picture. The go2rtc web UI works because MSE / WebRTC / MJPEG-over-HTTP all handle MJPEG.

### Fixes

1. **MJPEG camera in HA (simplest, live view only).** Skip RTSP; point HA at the MJPEG-over-HTTP endpoint:
   ```yaml
   camera:
     - platform: mjpeg
       name: Vacuum Camera
       mjpeg_url: http://<robot-ip>:1984/api/stream.mjpeg?src=camera
       still_image_url: http://<robot-ip>:1984/api/frame.jpeg?src=camera
   ```
   No recording/history (that needs H264).

2. **Transcode to H264 on the HA side (confirmed working, 2026-07-07).** HA ships go2rtc + ffmpeg (runs on the HA host, x86 — cheap); let it transcode while the robot stays MJPEG. In HA's go2rtc config:
   ```yaml
   streams:
     vacuum: ffmpeg:http://<robot-ip>:1984/api/stream.mjpeg?src=camera#video=h264
   ```
   Then add the `vacuum` stream as a go2rtc/WebRTC camera. This restores RTSP / stream / recording in HA.

3. **H264 on the robot (proper, more work).** Have `ava_cam_relay` emit H264 via the SoC CedarX encoder (`libvencoder.so`, see REVERSE_ENGINEERING §5a) so `rtsp://.../camera` natively offers H264 and HA consumes it directly over RTSP — no HA-side transcode.
