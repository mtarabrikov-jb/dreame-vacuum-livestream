# Build the Phase 2 aarch64 artifacts for the robot:
#   libcamtap.so    - LD_PRELOAD tap injected into ava (captures NV21 frames)
#   ava_cam_relay   - out-of-ava process: shm -> H264 (CedarX) -> :6969
#
# Toolchain: the aarch64 cross gcc (apt: gcc-aarch64-linux-gnu). Build inside the
# pinned Docker image (`make docker`) for a reproducible result and to avoid
# host-glibc symbol skew: dlopen/dlsym must resolve to <= GLIBC_2.23, which
# debian:bullseye's cross glibc (2.31) satisfies. A newer base would emit
# dlopen@GLIBC_2.34 and fail on the robot.

CC      := aarch64-linux-gnu-gcc
CFLAGS  := -O2 -Wall -Wextra
SHIM    := libcamtap.so
RELAY   := ava_cam_relay

.PHONY: all docker clean
all: $(SHIM) $(RELAY)

$(SHIM): src/libcamtap.c src/camtap_shm.h
	$(CC) $(CFLAGS) -fPIC -shared src/libcamtap.c -o $@ -ldl -lpthread
	@echo ">> built $(SHIM) (aarch64 LD_PRELOAD tap)"

$(RELAY): src/ava_cam_relay.c src/camtap_shm.h src/ir_process.h src/jpeg_gray.h
	$(CC) $(CFLAGS) src/ava_cam_relay.c -o $@ -lm
	@echo ">> built $(RELAY) (aarch64 encoder/relay)"

# Reproducible build with no host toolchain required (needs docker).
docker:
	docker build -t dreame-camstream-build .
	docker create --name dcs_tmp dreame-camstream-build >/dev/null
	docker cp dcs_tmp:/out/$(SHIM)  ./$(SHIM)
	docker cp dcs_tmp:/out/$(RELAY) ./$(RELAY)
	docker rm dcs_tmp >/dev/null
	@echo ">> docker build produced $(SHIM) + $(RELAY)"

clean:
	rm -f $(SHIM) $(RELAY)
