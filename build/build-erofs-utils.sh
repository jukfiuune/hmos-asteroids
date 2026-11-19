#!/bin/bash
# Builds erofs-utils (mkfs.erofs, fsck.erofs --extract) on the build PC into $HARMONY_BUILD/tools/erofs-utils/bin,
# for platform/qcom/android/build-android-helpers.sh and build/build-asteroids.sh. Needs lz4 and libuuid headers.
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
T=$HB/tools/erofs-utils
V=v1.8.2
mkdir -p $T
cd $T
[ -d src ] || git clone -q --depth 1 -b $V https://git.kernel.org/pub/scm/linux/kernel/git/xiang/erofs-utils.git src
cd src
[ -x configure ] || ./autogen.sh >/dev/null
[ -f Makefile ] || ./configure -q --prefix=$T --enable-lz4 --disable-fuse --without-selinux >/dev/null
make -s -j10 >/dev/null
make -s install >/dev/null
$T/bin/mkfs.erofs --version | head -1
