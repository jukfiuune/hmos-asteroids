#!/bin/bash
# Builds the camera's Android side on the build PC: camserver, the propd preload and camtool, which build-asteroids.sh
# puts in /asteroids (boot/svc/camera.sh runs them), and the CamX test root's tools (test/README):
#   logdw              logd stand-in (static OHOS binary), platform/qcom/android/logdw.c
#   socketexec         starts a daemon with init's sockets (static OHOS binary), test/socketexec.c; for tombstoned
#   libpropd.so        property area and property service for the Android root, platform/qcom/android/propd.c
#   libcamx_probe.so   loads CamX and lists the cameras, camx_probe.c
#   libcamserver.so    the camera bridge's server, bridge/camserver.c (a preload like the probe)
#   camtool            a client for it from M1's shell (static OHOS binary), bridge/camtool.c
#   system_ext.tgz     hwservicemanager and the system_ext VINTF manifests, out of Android's slot b system_ext
# The preloads link against bionic ($WORK/bionic-link, from android/build-android-helpers.sh). Output: $WORK/out-camera/.
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
N=$HB/ohos/sdk/native
O=$WORK/out-camera
mkdir -p $O
[ -f $WORK/bionic-link/libc.so ] || { echo "run android/build-android-helpers.sh first (bionic-link)"; exit 1; }
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -static -s \
    $QCOM/android/logdw.c -o $O/logdw
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -static -s \
    $QCOM/camera/test/socketexec.c -o $O/socketexec
BIONIC="--target=aarch64-linux-android33 --sysroot=$N/sysroot -isystem $N/sysroot/usr/include/aarch64-linux-ohos
    -fuse-ld=lld -fPIC -shared -nostdlib -D_GNU_SOURCE -mno-outline-atomics -O2 -Wall -Wextra -Wl,--no-undefined
    -L$WORK/bionic-link"
$N/llvm/bin/clang $BIONIC $QCOM/android/propd.c -lc -o $O/libpropd.so
$N/llvm/bin/clang $BIONIC $QCOM/camera/camx_probe.c -lc -ldl -o $O/libcamx_probe.so
$N/llvm/bin/clang $BIONIC $QCOM/camera/bridge/camserver.c -lc -lm -ldl -o $O/libcamserver.so
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -static -s \
    $QCOM/camera/bridge/camtool.c -o $O/camtool
X=$O/system_ext
rm -rf $X && $HB/tools/erofs-utils/bin/fsck.erofs --extract=$X $HB/android-b/system_ext_b.img
tar -czf $O/system_ext.tgz -C $X bin/hwservicemanager etc/vintf
rm -rf $X
ls -la $O/logdw $O/socketexec $O/libpropd.so $O/libcamx_probe.so $O/libcamserver.so $O/camtool $O/system_ext.tgz
