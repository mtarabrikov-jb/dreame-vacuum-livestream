#!/bin/sh
# ---------------------------------------------------------------------------
# go2rtc launcher. go2rtc pulls the raw H264 stream that whichever source
# (A or B) is publishing on 127.0.0.1:6969 and re-serves it as
# RTSP (:8554) / WebRTC (:8555) / MSE + web UI (:1984).
#
# It is started once by the supervisor and stays up across source switches;
# go2rtc reconnects to :6969 on its own when a producer (re)appears.
# ---------------------------------------------------------------------------
DIR="${REMOTE_DIR:-/data/camstream}"
cd "$DIR" || exit 1
exec ./go2rtc -config "$DIR/go2rtc.yaml"
