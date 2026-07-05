# Cross-compile the Phase 2 IPC relay to aarch64 for the robot.
#
# Toolchain (same as tihmstar/vacuumstreamer's Dockerfile):
#   apt install clang gcc-aarch64-linux-gnu
# clang uses its built-in aarch64 target; gcc-aarch64-linux-gnu supplies the
# sysroot headers + crt objects. No aarch64 gcc invocation is needed.
#
# We LINK against the robot's own libnanomsg.so.5 (fetched by `make pull-libs`)
# instead of dlopen'ing it. That keeps the binary free of any glibc symbol newer
# than the robot's 2.23 (notably dlopen@GLIBC_2.34 on modern build hosts) and
# bakes the correct DT_NEEDED (libnanomsg.so.5) for runtime.

TARGET     := aarch64-linux-gnu
CC         := clang
CFLAGS     := --target=$(TARGET) -O2 -Wall -Wextra
ROBOT      ?= root@ROBOT-IP-NOT-SET
ROBOT_LIBS := robot-libs
NANOMSG    := $(ROBOT_LIBS)/libnanomsg.so.5
BIN        := ava_cam_relay

.PHONY: all clean pull-libs

all: $(BIN)

$(BIN): src/ava_cam_relay.c $(NANOMSG)
	$(CC) $(CFLAGS) src/ava_cam_relay.c $(NANOMSG) -o $(BIN)
	@echo ">> built $(BIN) (aarch64). It is a SCAFFOLD — see phase2-cleaning/README.md"

# Fetch the robot's libnanomsg so the linker has the exact soname + symbols.
# Uses ssh+cat because the robot's BusyBox sshd has no sftp-server.
$(NANOMSG):
	@$(MAKE) pull-libs

pull-libs:
	@mkdir -p $(ROBOT_LIBS)
	@echo ">> pulling libnanomsg.so.5 from $(ROBOT)"
	@ssh $(ROBOT) 'cat "$$(find /usr/lib /lib -name libnanomsg.so.5 2>/dev/null | head -1)"' > $(NANOMSG)
	@test -s $(NANOMSG) || { echo "ERROR: could not fetch libnanomsg.so.5 (is ROBOT reachable?)"; rm -f $(NANOMSG); exit 1; }
	@echo ">> saved $(NANOMSG)"

clean:
	rm -f $(BIN)
