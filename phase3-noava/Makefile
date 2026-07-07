# Phase 3 (no-ava): standalone camera capture + Allwinner VIN media-controller
# RE tools. Cross-built for the robot (aarch64). `make docker` for a reproducible
# build with no host toolchain (needs docker). camtap_shm.h is shared with Phase 2.
CC      := aarch64-linux-gnu-gcc
CFLAGS  := -O2 -Wall -Wextra
SHM     := ../phase2-cleaning/src
TOOLS   := tools/media_topo tools/media_link tools/subdev_setfmt tools/subdev_probe \
           tools/query_ctrl tools/v4l2probe

.PHONY: all docker clean
all: src/w10-cam $(TOOLS)

# RGB standalone capture: dlopens libsunxicamera at runtime -> dynamic, -ldl.
# Only basic libc + dl symbols (<= GLIBC_2.17) so it loads on the robot's 2.23.
src/w10-cam: src/w10-cam.c $(SHM)/camtap_shm.h
	$(CC) $(CFLAGS) -I$(SHM) $< -o $@ -ldl
	@echo ">> built $@ (RGB standalone capture)"

# RE / pipeline-setup tools: static, self-contained.
tools/%: tools/%.c
	$(CC) $(CFLAGS) -static $< -o $@
	@echo ">> built $@"

docker:
	docker run --rm -v $(CURDIR)/..:/work -w /work/phase3-noava debian:bullseye sh -c '\
		apt-get update -qq >/dev/null && \
		apt-get install -y -qq gcc-aarch64-linux-gnu libc6-dev-arm64-cross make >/dev/null && \
		make all && \
		aarch64-linux-gnu-strip src/w10-cam $(TOOLS)'
	@echo ">> docker build done (aarch64)"

clean:
	rm -f src/w10-cam $(TOOLS)
