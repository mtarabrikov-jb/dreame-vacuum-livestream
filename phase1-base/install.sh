#!/bin/sh
# ---------------------------------------------------------------------------
# One-time on-robot setup / teardown for camstream.
#
#   install.sh            -> assemble overlays, hook boot persistence
#   install.sh uninstall  -> stop, unmount, remove boot hook
#
# All device-specific files (the /ava/conf tree, the /mnt/private cert/ULI) are
# assembled HERE, on the robot, from the robot's OWN filesystem. Nothing
# device-specific is ever shipped in the git repo.
# ---------------------------------------------------------------------------
set -u

DIR="${REMOTE_DIR:-/data/camstream}"
POSTBOOT=/data/_root_postboot.sh
BEGIN='# >>> camstream >>>'
END='# <<< camstream <<<'

assemble_overlays() {
	echo ">> assembling /ava/conf overlay"
	# Full copy of the read-only /ava/conf, PLUS the video_monitor subdir that
	# vacuumstreamer needs (video_monitor.cfg hardcodes /ava/conf/video_monitor).
	rm -rf "$DIR/ava_conf_ovl"
	mkdir -p "$DIR/ava_conf_ovl"
	cp -a /ava/conf/. "$DIR/ava_conf_ovl/"
	rm -rf "$DIR/ava_conf_ovl/video_monitor"
	cp -a "$DIR/ava_conf_video_monitor" "$DIR/ava_conf_ovl/video_monitor"

	echo ">> assembling /mnt/private overlay (device cert/ULI, from this robot)"
	rm -rf "$DIR/mnt_private_ovl"
	mkdir -p "$DIR/mnt_private_ovl"
	cp -a /mnt/private/. "$DIR/mnt_private_ovl/" 2>/dev/null || true
}

hook_persistence() {
	[ -f "$POSTBOOT" ] || { echo "WARN: $POSTBOOT missing — start manually with 'make start'"; return; }
	if grep -qF "$BEGIN" "$POSTBOOT"; then
		echo ">> boot hook already present in $POSTBOOT"
		return
	fi
	echo ">> adding boot hook to $POSTBOOT"
	{
		echo "$BEGIN"
		echo "[ -x $DIR/supervisor.sh ] && REMOTE_DIR=$DIR setsid sh $DIR/supervisor.sh >/data/log/camstream_supervisor.log 2>&1 < /dev/null &"
		echo "$END"
	} >> "$POSTBOOT"
	chmod +x "$POSTBOOT"
}

case "${1:-install}" in
	install)
		mkdir -p "$DIR" /data/log
		assemble_overlays
		hook_persistence
		echo ">> installed. Run 'make start' now (or reboot to start via boot hook)."
		;;
	uninstall)
		echo ">> stopping camstream"
		[ -x "$DIR/supervisor.sh" ] && REMOTE_DIR="$DIR" sh "$DIR/supervisor.sh" --stop 2>/dev/null || true
		echo ">> unmounting overlays"
		grep -q ' /ava/conf ' /proc/mounts && umount /ava/conf 2>/dev/null || true
		grep -q ' /mnt/private ' /proc/mounts && umount /mnt/private 2>/dev/null || true
		if [ -f "$POSTBOOT" ] && grep -qF "$BEGIN" "$POSTBOOT"; then
			echo ">> removing boot hook from $POSTBOOT"
			sed -i "/$BEGIN/,/$END/d" "$POSTBOOT"
		fi
		echo ">> done. Files remain in $DIR (rm -rf it manually to fully remove)."
		;;
	*)
		echo "usage: install.sh [install|uninstall]"; exit 1 ;;
esac
