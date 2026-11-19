#!/bin/bash
# Builds OpenHarmony's reference RIL vendor library (telephony_ril_adapter services/vendor, the code the donor's
# libril_vendor.z.so is built from) with patch-ril-vendor.py: both SIM slots through asteroids_atd, and the
# baseband version. Linked against the donor's hilog, securec and begetutil. Run on the build PC.
#   build-ril-vendor.sh <out dir>
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
N=$HB/ohos/sdk/native
W=$HB/modem
O=$HB/ohos/src
L=$WORK/ohos-link
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt/system/system
H=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$W/out}
R=$W/telephony_ril_adapter

[ -d $R ] || git clone -q https://gitee.com/openharmony/telephony_ril_adapter.git $R
git -C $R checkout -q -f e74589859b16bb576936608927eab2dc92a0497b
(cd $R && python3 $H/patch-ril-vendor.py)
[ -f $L/libbegetutil.z.so ] || sudo -n cat $DONOR/lib64/chipset-pub-sdk/libbegetutil.z.so > $L/libbegetutil.z.so

# parameter.h (init's) is only GetParameter and SetParameter here.
mkdir -p $OUT/shim
cat > $OUT/shim/parameter.h <<'EOF'
#ifndef ASTEROIDS_PARAMETER_H
#define ASTEROIDS_PARAMETER_H
int GetParameter(const char *key, const char *def, char *value, unsigned int len);
int SetParameter(const char *key, const char *value);
#endif
EOF
mkdir -p $OUT
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -fPIC -shared -fuse-ld=lld \
    -Wno-everything -DLOG_TAG='"RilVendor"' -DLOG_DOMAIN=0xD001F08 \
    -I$OUT/shim -I$R/services/vendor/include -I$R/interfaces/innerkits/include -I$R/utils/native/include \
    -I$O/hiviewdfx_hilog/interfaces/native/innerkits/include -I$O/third_party_bounds_checking_function/include \
    $R/services/vendor/src/*.c -Wl,-soname,libril_vendor.z.so -Wl,-z,relro,-z,now \
    $L/libhilog.so $L/libsec_shared.z.so $L/libbegetutil.z.so -o $OUT/libril_vendor.z.so
ls -la $OUT/libril_vendor.z.so
