# dreame-vacuum-livestream

Live camera from a **rooted Dreame Bot W10 (`r2104`)** robot vacuum, served through the robot's own
[go2rtc](https://github.com/AlexxIT/go2rtc) as **RTSP / WebRTC / MSE** (watch in a browser, VLC, or
Home Assistant). One stream whose content **auto-switches with the robot's state**:

| Robot state     | Feed | Source |
|-----------------|------|--------|
| **on the dock** | **full-color RGB** (the room from floor level) | the OV8856 camera, driven from inside `ava` |
| **cleaning**    | **grayscale infrared** (night-vision) | the ToF/depth sensor `ava` uses to see obstacles |

> **Model note.** Reverse-engineered on and tested against the **Dreame W10, `r2104`, SoC Allwinner
> `MR813`**. Other Dreame models share the stack but differ in details — see
> [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md) before assuming it ports.

---

## How it works

It's a single `LD_PRELOAD` shim (`libcamtap.so`) injected into `ava` (the robot's brain) plus a small
out-of-process relay — no cloud, no `video_monitor`, no second camera open:

- **On the dock** a background thread in the shim drives the RGB camera itself via the exported
  `sunxi_cam::SunxiCam` API (`OpenCamera` → `GetImageFrame` + `ReturnImageFrame` at ~14 fps) and copies
  each NV21 frame to a tmpfs buffer. It **yields the camera the moment cleaning starts**, so it never
  fights the robot's own use of it.
- **While cleaning** the shim taps the ToF sensor's stream (`/dev/video1`) that `ava` is already
  running for obstacle avoidance — the raw `224×1558` frame decodes into a clean infrared image.
- The **relay** reads whichever frame is in the buffer, encodes it to JPEG (a self-contained baseline
  encoder: color 4:2:0 for RGB, grayscale for IR), and serves MJPEG on loopback `:8090`. The robot's
  **existing go2rtc** consumes that and re-serves it to you.

The RGB camera being unavailable during cleaning — the spinning **LDS turret** disrupts the OV8856 MIPI
and wedges its ISP (isp0), so RGB and the LDS are mutually exclusive (it is the turret, not a firmware
idle policy) — and the ToF stream being the only live feed then, are both covered in the RE notes — as
is why the vendor's `video_monitor` path can't be used (it waits for a cloud "start" command the
de-clouded robot never sends).

---

## Status

**Working end-to-end**, verified live on the robot: a color kitchen frame on the dock and an infrared
frame while cleaning, both pulled from go2rtc's `api/frame.jpeg`. Deploy with `make build → upload →
install`, then `make watch` for the URLs. (An earlier attempt that reused the vendor `video_monitor`
for the dock view was dropped — it idles without a cloud trigger on a de-clouded robot; see
[`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md).)

---

## Requirements

**On your workstation** (to build & deploy):

- `make`, `bash`, `ssh`, `tar`, `curl`
- `docker` (preferred) **or** `gcc-aarch64-linux-gnu` + `libc6-dev-arm64-cross` — to cross-compile
  `libcamtap.so` + `ava_cam_relay`
- `ffmpeg` only if you want `make view`

**On the robot:**

- A **rooted** Dreame W10 (`r2104`) with SSH access (Valetudo / dustbuilder), `glibc 2.23`.
- A `go2rtc` binary — the dustbuilder/vacuumstreamer setup already puts one at
  `/data/vacuumstreamer/go2rtc`; otherwise `make build` downloads one and uploads it.

> The robot's BusyBox `sshd` has **no `sftp-server`**, so `scp -O` fails. Every transfer in this
> repo uses `ssh '… | cat > file'` / `tar | ssh 'tar -x'` instead. That's why there is no `scp`.

---

## Quick start

```bash
# 1. configure
cp config.mk.example config.mk
$EDITOR config.mk            # set ROBOT=root@<robot-ip>

# 2. cross-compile libcamtap.so + ava_cam_relay (Docker) and fetch go2rtc
make build

# 3. push everything to the robot (uses ssh+cat, not scp)
make upload

# 4. inject the shim into ava + start the feed (RESTARTS ava; robot idle on dock)
make install

# 5. watch it
make watch          # prints the URLs
```

Then open **`http://<robot-ip>:1984/`** (go2rtc web UI, stream `camera`) or point VLC at
**`rtsp://<robot-ip>:8554/camera`**. On the dock it's color RGB; start a cleaning and it switches to
infrared. `make uninstall` restores stock `ava`.

See [`docs/INSTALL.md`](docs/INSTALL.md) for the step‑by‑step walkthrough,
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design,
[`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) if the stream is dead (e.g. the camera stops mid-clean),
[`phase3-noava/README.md`](phase3-noava/README.md) for running the camera **without `ava`** (full-autonomy setups), and
[`Makefile`](Makefile) (`make help`) for every target.

---

## Credits

- [Dennis Giese / dontvacuum.me](https://dontvacuum.me) — Dreame rooting & research.
- [tihmstar/vacuumstreamer](https://github.com/tihmstar/vacuumstreamer) — the LD_PRELOAD-into-`ava`
  approach and the arm64 `go2rtc` build this reuses.
- [alufers/dreame_mcu_protocol](https://github.com/alufers/dreame_mcu_protocol) — MCU protocol reference.
- [go2rtc](https://github.com/AlexxIT/go2rtc) — the streaming server.

## License

MIT — see [`LICENSE`](LICENSE). Covers only *our* glue code and docs; go2rtc and any third-party
component keep their own licenses.
