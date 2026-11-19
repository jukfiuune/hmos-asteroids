#!/bin/bash
# Builds the GNSS vendor library over the modem's QMI LOC (asteroids_gnss.c) on the build PC:
#   vendorGnssAdapter.so  for location_host's gnss_interface_service (replaces the donor's, which drives Huawei's chip)
#   asteroids_gnss        the same code as a static test tool: runs a session from a shell and prints what arrives
# Output: $WORK/out-gnss/.
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
N=$HB/ohos/sdk/native
W=$HB/modem
L=$WORK/ohos-link
H=$(cd "$(dirname "$0")" && pwd)
O=$WORK/out-gnss
mkdir -p $O
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -Wno-unused-parameter -fuse-ld=lld"
# libqrtr from build-modem.sh's checkout, and linux/qrtr.h from pd-mapper (the NDK's clashes with musl's socket.h).
Q="-I$H/../audio/pd-mapper -I$W/qrtr/include $W/qrtr/lib/qrtr.c $W/qrtr/lib/logging.c"
$CC -fPIC -shared -fvisibility=hidden $H/asteroids_gnss.c $Q -Wl,-soname,vendorGnssAdapter.so -Wl,-z,relro,-z,now \
    $L/libhilog.so -o $O/vendorGnssAdapter.so 2>&1 | grep -v "qrtr/lib" || true
$CC -static -DGNSS_TEST $H/asteroids_gnss.c $Q -o $O/asteroids_gnss 2>&1 | grep -v "qrtr/lib" || true
ls -la $O/vendorGnssAdapter.so $O/asteroids_gnss
