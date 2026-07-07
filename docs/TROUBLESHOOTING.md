# Troubleshooting

## Camera dead — no frames, ISP `size 0` reset loop (`VIN_ERR` flood)

### Symptom

- The livestream freezes: go2rtc serves only the last frame from the moment it broke (a still image, not black), or nothing.
- `/tmp/camtap.shm` stops advancing (the tap has no frames to copy).
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

2. **When did frames stop?** `/tmp/camtap.shm` mtime is the last good frame (only advances while a feed is active — RGB on the dock, IR while cleaning):
   ```sh
   ls -la /tmp/camtap.shm ; date
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
If `/tmp/camtap.shm` still does not advance after that, the source (ava/sensor) is the problem, not the relay.

### What was ruled out (2026-07-07)

- **Software / our stack.** Not fixed by restarting `ava` (full V4L2 re-open + sensor re-init) nor by a cold reboot (kernel VIN/ISP/MIPI-PHY re-init from scratch — `size 0` reproduced from the first boot-time stream attempt). The tap (`libcamtap.so`) and relay (`ava_cam_relay` + go2rtc) were confirmed healthy by restarting them independently.
- **Thermal.** CPU/GPU/DDR ~66 C — no overheat.
- **Unrelated to the MCU/LDS work.** The robot had rebooted since; only `libcamtap.so` was loaded in `ava` (no `avatap.so`), and that work is on ttyS4/ttyS3 (a separate bus) with nothing on the MIPI/ISP path.

### The fix

Physical only. Power off, open the top cover, and **reseat the OV8856 camera FPC cable** at both ends (camera module and mainboard); inspect it for a crease/crack. Re-test with the diagnostic above — `dmesg | grep -c VIN_ERR` should stop growing and `/tmp/camtap.shm` should tick during a stream. If reseating does not fix it, replace the camera module (or, rarely, the mainboard MIPI connector).

### Optional mitigation until repaired

Until the cable is fixed, every cleaning makes `ava` burn a core retrying the dead sensor (camera-based obstacle avoidance is already non-functional — bumper, cliff and LDS still protect the robot). To stop the futile retries you can disable the camera/ToF node in `/ava/conf/r2104.conf` (config overlay). This is optional and only cosmetic (CPU/heat); it does not restore the camera.

### Reference (this robot, `r2104`)

- Sensors on I2C bus 3: `ov8856_mipi` @ 0x36 (RGB, "sensor0"), `ofilm0092` (ToF, "sensor1"), module EEPROM @ 0x50.
- OV8856 chip-ID: `0x300B = 0x88`, `0x300C = 0x56` (datasheet).
- VIN pipeline: `sunxi_mipi -> sunxi_csi -> sunxi_isp -> sunxi_scaler.0..3`; nodes `/dev/video0..2`. `ava` uses `/dev/video2` (cleaning), `video_monitor` uses `/dev/video0` (dock) — same physical OV8856.
- Feed switch: dock = RGB, cleaning = grayscale IR (see README) — both fail together on an OV8856/link fault.
