#!/bin/bash
# Builds two aarch64 Alpine trees with podman (qemu-user): sysroot/ for cross-compiling Mesa and the M2 tests with
# clang, and rootfs/ (runtime only) to run them on the phone in a chroot.
set -e
. "$(dirname "$0")/../../../build/env.sh"
cd $WORK
IMG=docker.io/library/alpine:3.22
build() {
    local name=$1; shift
    podman rm -f m2-$name >/dev/null 2>&1 || true
    podman run --arch arm64 --name m2-$name $IMG sh -c "apk add --no-cache $*"
    rm -rf $name $name.tar && podman export m2-$name -o $name.tar && mkdir $name && tar -xf $name.tar -C $name 2>/dev/null || true
    podman rm -f m2-$name >/dev/null; rm -f $name.tar
}
build sysroot musl-dev gcc g++ linux-headers libdrm-dev zlib-dev expat-dev zstd-dev vulkan-loader-dev vulkan-headers \
    libelf elfutils-dev
build rootfs libdrm vulkan-loader vulkan-tools libstdc++ zlib expat zstd-libs busybox-extras
du -sh sysroot rootfs
echo SYSROOTS-OK
