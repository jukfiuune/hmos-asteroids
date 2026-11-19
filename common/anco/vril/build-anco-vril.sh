#!/bin/bash
# Builds the host end of the Android container's virtual RIL (build PC, OpenHarmony NDK): anco_vrild, which runs
# Huawei's libril_msgtransfer.z.so on the virt_ril socket, and libanco_vril.z.so, the modem RIL behind it.
#   build-anco-vril.sh [out dir]
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
N=$HB/ohos/sdk/native
H=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$WORK/out-anco-vril}
mkdir -p $OUT
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -fuse-ld=lld -Wall -Wno-unused-parameter"
R=$HB/modem/telephony_ril_adapter/interfaces/innerkits/include # build-ril-vendor.sh's checkout
$CC -fPIE -pie -Wl,--export-dynamic -I$R $H/anco_vrild.c -ldl -lpthread -o $OUT/anco_vrild
$CC -fPIC -shared $H/anco_vril.c -Wl,-soname,libanco_vril.z.so -Wl,--allow-shlib-undefined -o $OUT/libanco_vril.z.so
ls -la $OUT/anco_vrild $OUT/libanco_vril.z.so
