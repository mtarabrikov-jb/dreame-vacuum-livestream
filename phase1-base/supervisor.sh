#!/bin/sh
# ---------------------------------------------------------------------------
# camstream supervisor  (Phase 1 + hook for Phase 2)
#
# Polls the robot's own Valetudo API for its movement state and decides which
# camera source should be feeding go2rtc:
#
#   state = docked            -> Source A  (video_monitor opens /dev/video0)
#   state = anything else     -> Source B  (ava_cam_relay, Phase 2) if present,
#                                otherwise NOTHING (camera released)
#
# WHY THIS EXISTS: the W10 has ONE physical camera. While cleaning, `ava` opens
# it (/dev/video2) for AI obstacle avoidance. If video_monitor ALSO holds the
# camera at that moment, the two capture pipelines collide on the sensor and the
# robot REBOOTS (reproducible). So the moment the robot leaves the dock we must
# kill Source A. See docs/REVERSE_ENGINEERING.md.
#
# go2rtc runs continuously; only the H264 producer on 127.0.0.1:6969 is switched.
# ---------------------------------------------------------------------------
set -u

DIR="${REMOTE_DIR:-/data/camstream}"
API="http://127.0.0.1/api/v2"
POLL="${POLL_INTERVAL:-4}"          # seconds between state polls
LOGDIR=/data/log
PIDFILE=/tmp/camstream_supervisor.pid
RELAY="$DIR/ava_cam_relay"          # Phase 2 binary (may be absent)

log() { echo "[$(date -u '+%Y-%m-%dT%H:%M:%SZ')] $*"; }

# --- current robot state via Valetudo (no jq on the robot) ------------------
# The attributes endpoint returns a JSON array; we isolate the
# StatusStateAttribute object and pull its "value".
robot_state() {
	curl -s --max-time 5 "$API/robot/state/attributes" \
		| sed 's/},{/}\n{/g' \
		| grep 'StatusStateAttribute' \
		| sed -n 's/.*"value":"\([a-z_]*\)".*/\1/p' \
		| head -n1
}

# --- process helpers --------------------------------------------------------
pids_of() { ps | grep -F "$1" | grep -v grep | awk '{print $1}'; }

start_go2rtc() {
	pids_of "$DIR/go2rtc" >/dev/null 2>&1 && pids_of "$DIR/go2rtc" | grep -q . && return 0
	log "starting go2rtc"
	setsid sh "$DIR/run_go2rtc.sh" >"$LOGDIR/go2rtc.log" 2>&1 < /dev/null &
}

start_source_a() {
	log "SOURCE A: video_monitor (camera open)"
	rm -f /tmp/videomonitor.socket
	REMOTE_DIR="$DIR" setsid sh "$DIR/run_vm.sh" >"$LOGDIR/video_monitor.log" 2>&1 < /dev/null &
}
stop_source_a() {
	for p in $(pids_of "$DIR/video_monitor"); do kill "$p" 2>/dev/null; done
	# give it a moment to release /dev/video0, then hard-kill leftovers
	for p in $(pids_of "$DIR/video_monitor"); do kill -9 "$p" 2>/dev/null; done
}

start_source_b() {
	if [ -x "$RELAY" ]; then
		log "SOURCE B: ava_cam_relay (passive IPC subscribe)"
		REMOTE_DIR="$DIR" setsid "$RELAY" >"$LOGDIR/ava_cam_relay.log" 2>&1 < /dev/null &
	else
		log "SOURCE B unavailable (Phase 2 relay not installed) -> no stream while off-dock"
	fi
}
stop_source_b() {
	for p in $(pids_of "$RELAY"); do kill "$p" 2>/dev/null; done
}

# --- one-time environment setup (idempotent) --------------------------------
# video_monitor.cfg hardcodes /ava/conf/video_monitor/recorder.cfg, but /ava is
# a read-only squashfs with no video_monitor dir. We bind-mount a writable copy
# of /ava/conf (assembled at install time) that includes that subdir. Same idea
# for /mnt/private (device cert/ULI that video_monitor's Agora init reads).
ensure_mounts() {
	if [ -d "$DIR/ava_conf_ovl" ] && ! grep -q ' /ava/conf ' /proc/mounts; then
		log "bind-mount overlay -> /ava/conf"
		mount --bind "$DIR/ava_conf_ovl" /ava/conf
	fi
	if [ -d "$DIR/mnt_private_ovl" ] && ! grep -q ' /mnt/private ' /proc/mounts; then
		log "bind-mount overlay -> /mnt/private"
		mount --bind "$DIR/mnt_private_ovl" /mnt/private
	fi
}

# ---------------------------------------------------------------------------
case "${1:-}" in
	--stop)
		[ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null
		rm -f "$PIDFILE"
		# stop every producer; leave go2rtc + mounts so --status still works
		for p in $(pids_of "$DIR/video_monitor") $(pids_of "$RELAY") $(pids_of "$DIR/go2rtc"); do
			kill "$p" 2>/dev/null
		done
		echo "camstream stopped"
		exit 0
		;;
	--status)
		st="$(robot_state)"; [ -z "$st" ] && st="(unreachable)"
		echo "robot state : $st"
		echo -n "supervisor  : "; [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && echo "running (pid $(cat "$PIDFILE"))" || echo "stopped"
		echo -n "source A    : "; pids_of "$DIR/video_monitor" | grep -q . && echo "video_monitor UP" || echo "down"
		echo -n "source B    : "; pids_of "$RELAY" | grep -q . && echo "ava_cam_relay UP" || echo "down"
		echo -n "go2rtc      : "; pids_of "$DIR/go2rtc" | grep -q . && echo "UP (:1984 web / :8554 rtsp)" || echo "down"
		echo -n "producer :6969: "; { netstat -ltn 2>/dev/null || ss -ltn 2>/dev/null; } | grep -q ':6969' && echo "listening" || echo "no"
		exit 0
		;;
esac

# --- main loop --------------------------------------------------------------
mkdir -p "$LOGDIR"
echo $$ > "$PIDFILE"
trap 'stop_source_a; stop_source_b; rm -f "$PIDFILE"; exit 0' INT TERM

ensure_mounts
start_go2rtc

mode=""            # "A" | "B" | ""
log "supervisor up (poll ${POLL}s), REMOTE_DIR=$DIR"

while true; do
	state="$(robot_state)"

	case "$state" in
		docked)          want="A" ;;
		"")              want="$mode" ;;   # API blip: hold current, don't thrash
		*)               want="B" ;;       # cleaning/returning/paused/moving/error/idle
	esac

	if [ "$want" != "$mode" ]; then
		log "state='$state' -> switch source '$mode' -> '$want'"
		case "$mode" in A) stop_source_a ;; B) stop_source_b ;; esac
		# small settle so the camera device is fully released before reopening
		sleep 1
		case "$want" in A) start_source_a ;; B) start_source_b ;; esac
		mode="$want"
	fi

	sleep "$POLL"
done
