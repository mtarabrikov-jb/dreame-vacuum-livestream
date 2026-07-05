#!/bin/sh
# ---------------------------------------------------------------------------
# Start (or stop) the Phase 2 infrared feed:
#   ava_cam_relay  — ToF frames from /tmp/camtap.shm -> grayscale IR -> MJPEG on
#                    127.0.0.1:8090 (loopback only; go2rtc is the sole consumer)
#   go2rtc         — consumes that and serves RTSP / WebRTC / MSE / snapshots
#
# Frames only flow while the robot is cleaning (that is when its ToF sensor
# streams). When idle the relay simply waits, so this is safe to leave running.
#
#   run_ir.sh          start relay + go2rtc
#   run_ir.sh --stop   stop both
# ---------------------------------------------------------------------------
DIR="${REMOTE_DIR:-/data/camstream}"
# Prefer an existing go2rtc on the robot; fall back to the one we shipped.
GO2RTC="${GO2RTC:-/data/vacuumstreamer/go2rtc}"
[ -x "$GO2RTC" ] || GO2RTC="$DIR/go2rtc"
mkdir -p /data/log

# Stop our relay and the go2rtc we launched (matched by our config path).
stop_feed() {
	killall ava_cam_relay 2>/dev/null
	for p in $(pidof go2rtc 2>/dev/null); do
		tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "go2rtc_ir.yaml" && kill "$p" 2>/dev/null
	done
}

if [ "$1" = "--stop" ]; then stop_feed; echo "IR feed stopped"; exit 0; fi

# always restart cleanly (avoids stale/duplicate processes)
stop_feed; sleep 1

setsid env IR_SCALE="${IR_SCALE:-2}" IR_BAND="${IR_BAND:--1}" IR_QUALITY="${IR_QUALITY:-80}" \
	"$DIR/ava_cam_relay" >/data/log/ir_relay.log 2>&1 < /dev/null &
sleep 1
setsid "$GO2RTC" -config "$DIR/go2rtc_ir.yaml" >/data/log/go2rtc.log 2>&1 < /dev/null &
sleep 1

IP=$(ip route get 1 2>/dev/null | awk '{print $NF; exit}')
echo "IR feed up. Watch it (only while cleaning):"
echo "  RTSP  : rtsp://${IP:-<robot-ip>}:8554/ircam"
echo "  Web   : http://${IP:-<robot-ip>}:1984/  (stream: ircam)"
echo "  Snap  : http://${IP:-<robot-ip>}:1984/api/frame.jpeg?src=ircam"
