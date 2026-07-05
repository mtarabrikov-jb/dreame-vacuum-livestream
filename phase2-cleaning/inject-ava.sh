#!/bin/sh
# ===========================================================================
# OPT-IN: inject libcamtap.so into the `ava` process (Phase 2, Source B).
#
#   !!! THIS RESTARTS ava, THE ROBOT'S NAVIGATION BRAIN. !!!
#   Run it only with the robot idle on the dock. If libcamtap.so has a bug,
#   ava may fail to start; recover with `inject-ava.sh remove` over SSH.
#
# Mechanism: /etc is a read-only squashfs and there is no /etc/ld.so.preload, so
# we bind-mount a tiny wrapper over /usr/bin/ava. ava.sh launches `ava`, which
# now resolves to our wrapper; the wrapper sets LD_PRELOAD=libcamtap.so and execs
# the real ava. libcamtap interposes libsunxicamera's GetImageFrame and copies
# each NV21 frame to /tmp/camtap.shm for ava_cam_relay. Nothing else changes.
#
#   inject-ava.sh install   set it up + restart ava + persist across reboot
#   inject-ava.sh remove     tear it down + restart stock ava
#   inject-ava.sh boot       (called from _root_postboot.sh) apply at boot
#   inject-ava.sh status     show whether the tap is active in ava
# ===========================================================================
set -u
DIR="${REMOTE_DIR:-/data/camstream}"
# The real ava copy MUST keep basename "ava" so its process comm stays "ava" —
# otherwise ava.sh's `killall -9 ava` and sys_monitor's `pidof ava` miss it and
# spawn a duplicate. Hence a dir, not "ava.real".
REALDIR="$DIR/ava_real"
REAL="$REALDIR/ava"
WRAP="$DIR/ava.wrap"
POSTBOOT=/data/_root_postboot.sh
BEGIN='# >>> camstream-phase2 >>>'
END='# <<< camstream-phase2 <<<'

ava_pid() { pidof ava 2>/dev/null; }
tap_active() { p=$(ava_pid); [ -n "$p" ] && grep -q libcamtap "/proc/$p/maps" 2>/dev/null; }

make_wrapper() {
	# snapshot the real ava ONCE, before any bind mount can hide it
	if ! grep -q ' /usr/bin/ava ' /proc/mounts && [ ! -f "$REAL" ]; then
		mkdir -p "$REALDIR"
		cp -a /usr/bin/ava "$REAL"
	fi
	cat > "$WRAP" <<EOF
#!/bin/sh
export LD_PRELOAD=$DIR/libcamtap.so
exec $REAL "\$@"
EOF
	chmod +x "$WRAP" "$REAL"
}

bind_on()  { grep -q ' /usr/bin/ava ' /proc/mounts || mount --bind "$WRAP" /usr/bin/ava; }
bind_off() { grep -q ' /usr/bin/ava ' /proc/mounts && umount /usr/bin/ava; }
restart_ava() { echo ">> restarting ava"; /etc/rc.d/ava.sh >/dev/null 2>&1 & sleep 3; }

persist_on() {
	[ -f "$POSTBOOT" ] || return
	grep -qF "$BEGIN" "$POSTBOOT" && return
	{ echo "$BEGIN"; echo "[ -x $DIR/inject-ava.sh ] && REMOTE_DIR=$DIR sh $DIR/inject-ava.sh boot"; echo "$END"; } >> "$POSTBOOT"
}
persist_off() { [ -f "$POSTBOOT" ] && sed -i "/$BEGIN/,/$END/d" "$POSTBOOT"; }

case "${1:-status}" in
	install)
		[ -f "$DIR/libcamtap.so" ] || { echo "ERROR: $DIR/libcamtap.so missing (build + upload Phase 2 first)"; exit 1; }
		make_wrapper; bind_on; persist_on; restart_ava
		if tap_active; then echo ">> OK: libcamtap active in ava. Start cleaning, then check /tmp/camtap.shm."; else echo ">> WARN: tap not detected in ava maps yet; check 'ps | grep ava' and logs."; fi
		;;
	remove)
		persist_off; bind_off; restart_ava
		tap_active && echo ">> WARN: tap still present" || echo ">> removed; stock ava running"
		;;
	boot)
		make_wrapper; bind_on
		# if ava already came up stock (postboot ran after ava.sh), reload it once
		tap_active || restart_ava
		;;
	status)
		echo -n "bind mount : "; grep -q ' /usr/bin/ava ' /proc/mounts && echo yes || echo no
		echo -n "ava pid    : "; ava_pid || echo none
		echo -n "tap active : "; tap_active && echo YES || echo no
		echo -n "shm frames : "; [ -e /tmp/camtap.shm ] && echo "present" || echo "none (only during cleaning)"
		;;
	*) echo "usage: inject-ava.sh install|remove|boot|status"; exit 1 ;;
esac
