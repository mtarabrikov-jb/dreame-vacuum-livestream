# Reproducible cross-build of the Phase 2 aarch64 artifacts.
#
# Pinned to bullseye ON PURPOSE: its aarch64 cross glibc (2.31) still keeps
# dlopen/dlsym in libdl (versioned <= GLIBC_2.17), so the binaries run on the
# robot's glibc 2.23. A newer base (glibc >= 2.34) would emit dlopen@GLIBC_2.34
# and fail there.
FROM debian:bullseye

RUN apt-get update && \
    apt-get install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross make ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# Single source of truth: the Makefile builds both artifacts with the cross gcc.
RUN make all && mkdir -p /out && cp libcamtap.so ava_cam_relay /out/
