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

The RGB camera being off during cleaning, and the ToF stream being the only live feed then, are both
covered in the RE notes — as is why the vendor's `video_monitor` path can't be used (it waits for a
cloud "start" command the de-clouded robot never sends).

---

## Status

**Working end-to-end**, verified live on the robot: a color kitchen frame on the dock and an infrared
frame while cleaning, both pulled from go2rtc's `api/frame.jpeg`. Deploy with `make build → upload →
install-phase2`, then `make watch` for the URLs. (A separate, now-superseded Phase 1 that used
`video_monitor` for the dock view is kept in the tree but not needed — `video_monitor` idles without
a cloud trigger on a de-clouded robot.)

---

## Requirements

**On your workstation** (to build & deploy):

- `make`, `bash`, `ssh`, `tar`, `curl`
- `docker` (preferred) **or** `gcc-aarch64-linux-gnu` + `libc6-dev-arm64-cross` — only for Phase 2 (`make build-phase2`)
- A checkout of [tihmstar/vacuumstreamer](https://github.com/tihmstar/vacuumstreamer) **already built**
  (provides Source A: `video_monitor`, `vacuumstreamer.so`, and the `dist/ava/conf/video_monitor` configs).
  Point `VACUUMSTREAMER_DIR` at it in `config.mk`.
  *We do not redistribute `video_monitor` — it is a proprietary Dreame binary you extract yourself.*

**On the robot:**

- A **rooted** Dreame W10 with SSH access (Valetudo / dustbuilder).
- `glibc 2.23`, `libagora-rtc-sdk.so`, `libvencoder.so` present (stock W10 has all of these).

> The robot's BusyBox `sshd` has **no `sftp-server`**, so `scp -O` fails. Every transfer in this
> repo uses `ssh '… | cat > file'` / `tar | ssh 'tar -x'` instead. That's why there is no `scp`.

---

## Quick start

```bash
# 1. configure
cp config.mk.example config.mk
$EDITOR config.mk            # set ROBOT=root@<robot-ip> and VACUUMSTREAMER_DIR

# 2. build local artifacts (go2rtc download; Phase 2 binary if you have clang)
make build

# 3. push everything to the robot (uses ssh+cat, not scp)
make upload

# 4. install: sets up config overlay + persistence in /data/_root_postboot.sh
make install

# 5. start the supervisor
make start

# 6. watch it
make status         # shows robot state + which source is live + the URLs
```

Then open **`http://<robot-ip>:1984`** (go2rtc web UI) or point VLC at
**`rtsp://<robot-ip>:8554/camera`**.

`make stop` / `make uninstall` cleanly tear everything down (unmounts, removes the postboot block).

See [`docs/INSTALL.md`](docs/INSTALL.md) for a step‑by‑step walkthrough and
[`Makefile`](Makefile) (`make help`) for every target.

---

## How it works (short version)

```
                 supervisor.sh  (polls Valetudo /api/v2/robot/state)
                        │
        docked/idle ────┤──── cleaning/moving
                        │
   ┌────────────────────┴─────────────────────┐
   │ Source A                                  │ Source B  (Phase 2)
   │ video_monitor (opens /dev/video0)         │ libcamtap.so tap inside ava
   │  + vacuumstreamer.so  ── H264 ──▶ :6969   │  (SunxiCam::GetImageFrame) ─NV21─▶
   │  (killed before cleaning starts)          │  ava_cam_relay ── H264 ──▶ :6969
   └────────────────────┬─────────────────────┘
                        ▼
                    go2rtc  ──▶  RTSP :8554 / WebRTC :8555 / Web :1984
```

The full design, the camera topology, the IPC bus format, and *why the reboot happens* are documented
in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md).

---

## Credits

- [tihmstar/vacuumstreamer](https://github.com/tihmstar/vacuumstreamer) — Source A (`video_monitor` Agora hook).
- [Dennis Giese / dontvacuum.me](https://dontvacuum.me) — Dreame rooting & research.
- [alufers/dreame_mcu_protocol](https://github.com/alufers/dreame_mcu_protocol) — MCU protocol reference used to sanity‑check the IPC framing.
- [go2rtc](https://github.com/AlexxIT/go2rtc) — the streaming server.

## License

MIT — see [`LICENSE`](LICENSE). Note this only covers *our* glue code and docs; the Dreame
`video_monitor` binary you supply separately is proprietary.
