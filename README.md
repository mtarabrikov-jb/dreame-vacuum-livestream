# dreame-vacuum-livestream

Live camera streaming from a **rooted Dreame Bot W10 (`r2104`)** robot vacuum, that works
**both on the dock and during cleaning**, without breaking the robot's AI navigation.

The stream is exposed as **RTSP / WebRTC / MSE** via [go2rtc](https://github.com/AlexxIT/go2rtc),
so you can watch it in a browser, VLC, or Home Assistant.

> **Model note.** Everything here was reverse‑engineered on and tested against the **Dreame W10,
> product code `r2104`, SoC Allwinner `MR813`, sensor `OV8856`**. Other Dreame models use similar
> but not identical internals — see [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md)
> before assuming it ports.

---

## Why this exists

The obvious approach — [tihmstar/vacuumstreamer](https://github.com/tihmstar/vacuumstreamer) — runs a
stand‑alone `video_monitor` binary that **opens the camera directly** (`/dev/video0`). That works on
the L10s Ultra (`r2228`), and it works on the W10 **while the robot is idle**. But the W10 has a
**single physical camera** that the robot also needs for AI obstacle avoidance while cleaning.

If `video_monitor` holds the camera while the robot starts cleaning, the two capture paths fight over
the same sensor and **the robot reboots** (observed, reproducible — see the RE notes). So a naive
"just leave it running" setup is unsafe.

This project solves that with **two sources and a supervisor that switches between them**:

| Robot state        | Source                                        | Camera owner |
|--------------------|-----------------------------------------------|--------------|
| docked / idle      | **A:** `video_monitor` opens the camera       | `video_monitor` |
| cleaning / moving  | **B:** subscribe to the frames `ava` already publishes on its internal bus | `ava` (robot) |

Because during cleaning we **don't open the camera a second time** — we passively read the frames
`ava` is already producing for its AI node — there is **no conflict and no reboot**.

---

## Status

| Phase | What it does | State |
|-------|--------------|-------|
| **Phase 1** — supervisor + Source A | Safe base viewing. Auto‑starts `video_monitor` on the dock, **auto‑kills it before cleaning** so the robot never reboots. | ✅ **Working** (`make` targets below) |
| **Phase 2** — Source B (cleaning) | Passive subscriber to `ava`'s `ava_ai_camera_msg` on the IPC bus → H264 → go2rtc. | 🚧 **Framework + RE done, encoder/decode `TODO`** — see [`phase2-cleaning/README.md`](phase2-cleaning/README.md) |

With Phase 1 alone you get a **safe, automatic** stream that you can watch from the dock and that
gets out of the way during cleaning. Phase 2 fills the "during cleaning" gap; the protocol is
documented and the subscriber is scaffolded, but the NV21→H264 encode step still needs finishing.

---

## Requirements

**On your workstation** (to build & deploy):

- `make`, `bash`, `ssh`, `tar`, `curl`
- `clang` + `gcc-aarch64-linux-gnu` (cross toolchain for Phase 2) — only needed for `make build-phase2`
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
   │ video_monitor (opens /dev/video0)         │ ava_cam_relay (subscribes to
   │  + vacuumstreamer.so  ── H264 ──▶ :6969   │  ipc:///tmp/avamsg.socket,
   │  (killed before cleaning starts)          │  ava_ai_camera_msg) ─ H264 ─▶ :6969
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
