#!/bin/sh
# ---------------------------------------------------------------------------
# Source A launcher: Dreame `video_monitor` + the vacuumstreamer Agora hook.
#
# vacuumstreamer.so is an LD_PRELOAD shim over the Agora RTC SDK: it intercepts
# setQueueH264Buffer() and diverts the already-encoded H264 stream to TCP
# 127.0.0.1:6969 instead of the cloud. go2rtc then serves it as RTSP/WebRTC.
#
# This MUST be a dedicated exec-launcher. video_monitor has a singleton guard
# (`ps | grep video_monitor`); if the launching process still has
# "video_monitor" in its argv when the guard runs, it aborts with
# "video_monitor is Running, Please Check". `exec` replaces this shell with the
# binary, so exactly one matching process exists.
# ---------------------------------------------------------------------------
DIR="${REMOTE_DIR:-/data/camstream}"

export LD_PRELOAD="$DIR/vacuumstreamer.so"
cd "$DIR" || exit 1

# A stale unix socket from a previous run makes video_monitor exit immediately.
rm -f /tmp/videomonitor.socket

exec ./video_monitor
