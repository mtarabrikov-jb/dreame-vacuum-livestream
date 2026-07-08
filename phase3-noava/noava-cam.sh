#!/bin/sh
# ===========================================================================
# Standalone camera WITHOUT ava (Phase 3). Brings up the camera feed(s) via
# w10-cam -> /tmp/camtap*.shm -> ava_cam_relay -> go2rtc, so a full-autonomy
# stack (e.g. SangamIO driving the MCU/LDS) can also serve the camera.
#
#   !!! Requires `ava` to be STOPPED already (the sensor must be free). Use it
#       together with sangamio's w10-direct.sh, or stop ava yourself first.
#
#   noava-cam.sh start        RGB only     (color, /dev/video2 -> go2rtc "camera")
#   noava-cam.sh start tof    IR/ToF only  (gray,  /dev/video1 -> go2rtc "camera")
#   noava-cam.sh start both   BOTH at once (RGB -> "camera", IR/ToF -> "camera_ir")
#   noava-cam.sh stop
#   noava-cam.sh status
#
# The two sensors have SEPARATE ISPs (isp0 for RGB, isp1 for ToF) so they can
# capture concurrently. `both` runs two w10-cam + two ava_cam_relay (one per shm,
# ports 8090/8091) feeding a single go2rtc that serves both as named streams:
#   RTSP rtsp://<ip>:8554/camera   +   rtsp://<ip>:8554/camera_ir
# ===========================================================================
set -u
BIN="${CAM_BIN:-/data/camstream}"          # w10-cam + media tools
RELAYDIR="${CAM_RELAY:-/data/camstream}"   # ava_cam_relay, go2rtc, go2rtc_ir.yaml
SHM=/tmp/camtap.shm                        # RGB / single feed  -> relay :8090 -> "camera"
SHM_IR=/tmp/camtap_ir.shm                  # second feed (both) -> relay :8091 -> "camera_ir"
GO2RTC="${GO2RTC:-/data/vacuumstreamer/go2rtc}"; [ -x "$GO2RTC" ] || GO2RTC="$RELAYDIR/go2rtc"

tof_pipeline() {   # enable the two ToF links + set every pad on the path to 224x1558/0x3011
    "$BIN/media_link" /dev/media0 1 0 32 0 "$1" >/dev/null 2>&1
    "$BIN/media_link" /dev/media0 26 1 44 0 "$1" >/dev/null 2>&1
    [ "$1" = 1 ] || return 0
    for p in "7 0" "7 1" "5 0" "5 1" "11 0" "11 2" "14 0" "14 1"; do
        "$BIN/subdev_setfmt" /dev/v4l-subdev${p% *} ${p#* } 224 1558 3011 >/dev/null 2>&1
    done
}

cam_rgb() {   # $1 = shm path. OpenCamera(/dev/video2) brings up the RGB path itself.
    setsid env CAM_INDEX=2 CAM_SHM="$1" \
        "$BIN/w10-cam" >/data/log/w10cam.log 2>&1 </dev/null &
}
cam_tof() {   # $1 = shm path. Raw BG12 path needs the media links + pad formats set first.
    tof_pipeline 1
    setsid env CAM_INDEX=1 CAM_W=224 CAM_H=1558 CAM_FOURCC=BG12 CAM_FRAMESZ=698368 \
        CAM_FORMAT=100 CAM_SHM="$1" "$BIN/w10-cam" >/data/log/w10cam_ir.log 2>&1 </dev/null &
}
relay() {     # $1 = shm path, $2 = MJPEG port. Relay auto-detects NV21 vs ToF by shm format.
    setsid env CAM_SHM="$1" IR_PORT="$2" IR_SCALE="${IR_SCALE:-2}" \
        "$RELAYDIR/ava_cam_relay" >"/data/log/relay_$2.log" 2>&1 </dev/null &
}
go2rtc_up() { # one go2rtc for all streams; replace any prior instance on this config
    for p in $(pidof go2rtc 2>/dev/null); do
        tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q go2rtc_ir.yaml && kill "$p" 2>/dev/null
    done
    setsid "$GO2RTC" -config "$RELAYDIR/go2rtc_ir.yaml" >/data/log/go2rtc.log 2>&1 </dev/null &
    sleep 1
}

start_feed() {   # $1 = rgb | tof | both
    # Free the sensors and release :8090/:8091 (a boot-time relay may hold them).
    killall w10-cam ava_cam_relay 2>/dev/null; sleep 2
    case "${1:-rgb}" in
        tof)  cam_tof "$SHM";                    sleep 2; relay "$SHM"    8090 ;;
        both) cam_rgb "$SHM"; sleep 1; cam_tof "$SHM_IR"; sleep 2
              relay "$SHM" 8090; relay "$SHM_IR" 8091 ;;
        *)    cam_rgb "$SHM";                    sleep 2; relay "$SHM"    8090 ;;
    esac
    sleep 1
    go2rtc_up
}

case "${1:-status}" in
    start)
        [ -x "$BIN/w10-cam" ] || { echo "ERROR: $BIN/w10-cam missing"; exit 1; }
        pidof ava >/dev/null && { echo "ERROR: ava is running -- stop it first (w10-direct.sh start)"; exit 1; }
        MODE="${2:-rgb}"
        start_feed "$MODE"
        IP=$(ip route get 1 2>/dev/null | awk '{print $NF; exit}')
        if pidof w10-cam >/dev/null; then
            if [ "$MODE" = both ]; then
                echo ">> both feeds up. RGB rtsp://${IP:-<ip>}:8554/camera  IR/ToF rtsp://${IP:-<ip>}:8554/camera_ir  Web http://${IP:-<ip>}:1984/"
                tail -2 /data/log/w10cam.log 2>/dev/null; tail -2 /data/log/w10cam_ir.log 2>/dev/null
            else
                echo ">> $MODE feed up. RTSP rtsp://${IP:-<ip>}:8554/camera  Web http://${IP:-<ip>}:1984/"
                tail -2 /data/log/w10cam.log 2>/dev/null
            fi
        else
            echo ">> WARN: w10-cam not running -- see /data/log/w10cam*.log"; tail -6 /data/log/w10cam.log 2>/dev/null
        fi
        ;;
    stop)
        killall w10-cam ava_cam_relay 2>/dev/null
        for p in $(pidof go2rtc 2>/dev/null); do
            tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q go2rtc_ir.yaml && kill "$p" 2>/dev/null
        done
        tof_pipeline 0   # disable ToF links (harmless if RGB only)
        echo ">> camera stopped"
        ;;
    status)
        echo -n "w10-cam : "; pidof w10-cam || echo none
        echo -n "relay   : "; pidof ava_cam_relay || echo none
        echo -n "go2rtc  : "; pidof go2rtc || echo none
        echo -n "RGB   shm frames: "; dd if=$SHM    bs=1 skip=24 count=8 2>/dev/null | od -An -tu8 | tr -d ' '
        echo -n "IR/ToF shm frames: "; dd if=$SHM_IR bs=1 skip=24 count=8 2>/dev/null | od -An -tu8 | tr -d ' '
        ;;
    *) echo "usage: noava-cam.sh start [tof|both] | stop | status"; exit 1 ;;
esac
