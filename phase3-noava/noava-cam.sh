#!/bin/sh
# ===========================================================================
# Standalone camera WITHOUT ava (Phase 3). Brings up one feed (RGB or IR/ToF)
# via w10-cam -> /tmp/camtap.shm -> ava_cam_relay -> go2rtc, so a full-autonomy
# stack (e.g. SangamIO driving the MCU/LDS) can also serve the camera.
#
#   !!! Requires `ava` to be STOPPED already (the sensor must be free). Use it
#       together with sangamio's w10-direct.sh, or stop ava yourself first.
#
#   noava-cam.sh start        RGB (color, /dev/video2)
#   noava-cam.sh start tof    IR/ToF (grayscale, /dev/video1 -- sets up its pipeline)
#   noava-cam.sh stop
#   noava-cam.sh status
#
# Only one feed at a time (the relay reads a single /tmp/camtap.shm). The two
# sensors CAN run at once (separate ISPs) -- to stream both, run a second
# relay+go2rtc on another shm; not done here.
# ===========================================================================
set -u
BIN="${CAM_BIN:-/data/camstream}"          # w10-cam + media tools
RELAYDIR="${CAM_RELAY:-/data/camstream}"   # ava_cam_relay, go2rtc, go2rtc_ir.yaml
SHM=/tmp/camtap.shm
GO2RTC="${GO2RTC:-/data/vacuumstreamer/go2rtc}"; [ -x "$GO2RTC" ] || GO2RTC="$RELAYDIR/go2rtc"

tof_pipeline() {   # enable the two ToF links + set every pad on the path to 224x1558/0x3011
    "$BIN/media_link" /dev/media0 1 0 32 0 "$1" >/dev/null 2>&1
    "$BIN/media_link" /dev/media0 26 1 44 0 "$1" >/dev/null 2>&1
    [ "$1" = 1 ] || return 0
    for p in "7 0" "7 1" "5 0" "5 1" "11 0" "11 2" "14 0" "14 1"; do
        "$BIN/subdev_setfmt" /dev/v4l-subdev${p% *} ${p#* } 224 1558 3011 >/dev/null 2>&1
    done
}

start_feed() {
    killall w10-cam 2>/dev/null; sleep 1
    if [ "${1:-rgb}" = tof ]; then
        tof_pipeline 1
        setsid env CAM_INDEX=1 CAM_W=224 CAM_H=1558 CAM_FOURCC=BG12 CAM_FRAMESZ=698368 \
            CAM_FORMAT=100 CAM_SHM=$SHM "$BIN/w10-cam" >/data/log/w10cam.log 2>&1 </dev/null &
    else
        setsid env CAM_INDEX=2 CAM_SHM=$SHM "$BIN/w10-cam" >/data/log/w10cam.log 2>&1 </dev/null &
    fi
    sleep 2
    # relay + go2rtc (matched-by-config, like run_ir.sh). Wait for the old relay
    # (from ava's boot) to release :8090 before the new one binds.
    killall ava_cam_relay 2>/dev/null; sleep 2
    setsid env IR_SCALE="${IR_SCALE:-2}" "$RELAYDIR/ava_cam_relay" >/data/log/ir_relay.log 2>&1 </dev/null &
    sleep 1
    for p in $(pidof go2rtc 2>/dev/null); do
        tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q go2rtc_ir.yaml && kill "$p" 2>/dev/null
    done
    setsid "$GO2RTC" -config "$RELAYDIR/go2rtc_ir.yaml" >/data/log/go2rtc.log 2>&1 </dev/null &
    sleep 1
}

case "${1:-status}" in
    start)
        [ -x "$BIN/w10-cam" ] || { echo "ERROR: $BIN/w10-cam missing"; exit 1; }
        pidof ava >/dev/null && { echo "ERROR: ava is running -- stop it first (w10-direct.sh start)"; exit 1; }
        start_feed "${2:-rgb}"
        IP=$(ip route get 1 2>/dev/null | awk '{print $NF; exit}')
        if pidof w10-cam >/dev/null; then
            echo ">> ${2:-rgb} feed up. RTSP rtsp://${IP:-<ip>}:8554/camera  Web http://${IP:-<ip>}:1984/"
            tail -2 /data/log/w10cam.log 2>/dev/null
        else
            echo ">> WARN: w10-cam not running -- see /data/log/w10cam.log"; tail -6 /data/log/w10cam.log 2>/dev/null
        fi
        ;;
    stop)
        killall w10-cam ava_cam_relay 2>/dev/null
        for p in $(pidof go2rtc 2>/dev/null); do
            tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q go2rtc_ir.yaml && kill "$p" 2>/dev/null
        done
        tof_pipeline 0   # disable ToF links (harmless if RGB)
        echo ">> camera stopped"
        ;;
    status)
        echo -n "w10-cam : "; pidof w10-cam || echo none
        echo -n "relay   : "; pidof ava_cam_relay || echo none
        echo -n "go2rtc  : "; pidof go2rtc || echo none
        echo -n "shm frames: "; dd if=$SHM bs=1 skip=24 count=8 2>/dev/null | od -An -tu8 | tr -d ' '
        ;;
    *) echo "usage: noava-cam.sh start [tof] | stop | status"; exit 1 ;;
esac
