#!/bin/bash
# Builds a software PIN backend for the donor's pin_auth_host (build PC), as useriam/build-useriam.sh does for user
# auth. The donor's libpin_auth_interface_service_3.0.z.so keeps PINs in Huawei's TEE (it links their
# libpinauth_daemon), which this phone doesn't have, so no PIN could be set. OpenHarmony's own implementation of the
# same interface (drivers_peripheral/pin_auth/hdi_service, PIN interface 3.0, as the donor's) keeps them in software
# under /data/service/el1/public/pinauth; it replaces the donor's library in the /vendor overlay. The donor's driver
# (libpin_auth_driver.z.so) and stub stay: the driver only takes PinAuthInterfaceImplGetInstance from it, which
# pin_auth_init.cpp wraps with the setup OpenHarmony's driver would do (PinAuth::Init).
# Headers for interface 3.0 come from hdi-gen. Output: $WORK/out-pinauth/.
# OpenSSL: the donor ships 3.0.9; the host's 3.x headers are API-compatible for the calls used here (as for useriam).
set -e
. "$(dirname "$0")/../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
N=$HB/ohos/sdk/native
S=$HB/ohos/src
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt
O=$WORK/out-pinauth
L=$O/link
G=$O/gen
H=$(cd "$(dirname "$0")" && pwd)
mkdir -p $L
# OpenHarmony's sources, copied, with our patches (*.patch here) on top.
A=$O/src/hdi_service
rm -rf $O/src && mkdir -p $O/src && cp -r $S/drivers_peripheral/pin_auth/hdi_service $S/drivers_peripheral/pin_auth/common $O/src/
for p in $H/*.patch; do patch -s -d $A -p1 < $p; done
for f in system/system/lib64/chipset-sdk-sp/{libc++.so,libutils.z.so,libhdf_utils.z.so,libhdi.z.so,libcrypto_openssl.z.so,libsec_shared.z.so} \
         system/system/lib64/chipset-pub-sdk/{libhilog.so,libipc_single.z.so,libbegetutil.z.so} \
         vendor/lib64/libpin_auth_stub_3.0.z.so; do
    [ -f $L/$(basename $f) ] || sudo -n cat $DONOR/$f > $L/$(basename $f)
done

HG=$S/drivers_hdf_core/framework/tools/hdi-gen/hdi-gen
I=$O/idl/ohos/hdi/pin_auth
rm -rf $O/idl $G && mkdir -p $I $G
cp -r $S/drivers_interface/pin_auth/v3_0 $I/
$HG -s full -m ipc -l cpp -r ohos.hdi:$O/idl/ohos/hdi $(for f in $I/v3_0/*.idl; do echo -c $f; done) -d $G

# init's parameter.h and sysparam_errno.h (its sources aren't here): pin_auth.cpp only uses GetDevUdid (in the
# donor's libbegetutil) and EC_SUCCESS.
mkdir -p $O/shim
cat > $O/shim/parameter.h <<'SHIM'
#ifndef ASTEROIDS_PARAMETER_H
#define ASTEROIDS_PARAMETER_H
#ifdef __cplusplus
extern "C" {
#endif
int GetDevUdid(char *udid, int size);
#ifdef __cplusplus
}
#endif
#endif
SHIM
printf '#ifndef ASTEROIDS_SYSPARAM_ERRNO_H\n#define ASTEROIDS_SYSPARAM_ERRNO_H\n#define EC_SUCCESS 0\n#endif\n' \
    > $O/shim/sysparam_errno.h

HDF=$S/drivers_hdf_core/interfaces/inner_api
mkdir -p $O/openssl-include && cp -r /usr/include/openssl $O/openssl-include/
INC="-I$O/shim -I$O/openssl-include -I$G -I$G/pin_auth -I$A/service/inc -I$A/adaptor/inc -I$A/common/inc
     -I$A/database/inc -I$A/main/inc -I$A/../common/logs -I$A/../common/utils -I$S/third_party_bounds_checking_function/include
     -I$S/hiviewdfx_hilog/interfaces/native/innerkits/include -I$S/commonlibrary_c_utils/base/include
     -I$S/communication_ipc/interfaces/innerkits/ipc_core/include -I$HDF/utils -I$HDF/hdi -I$HDF/hdi/base
     -I$HDF/host/shared -I$HDF/core -I$HDF/ipc -I$HDF/osal/uhdf -I$HDF/osal/shared"
CF="--target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -fPIC -Wall -Wextra -Wno-unused-parameter
        -Wno-deprecated-declarations $INC"
# pin_auth.cpp narrows vector sizes in initializer lists, which OpenHarmony's build lets through.
CPP="-fno-rtti -std=c++17 -nostdlib++ -Wno-c++11-narrowing -include $COMMON/include/platform_cpp_config.h"
CC=$N/llvm/bin/clang; CXX=$N/llvm/bin/clang++
mkdir -p $O/obj
objs=()
for src in $(sed -n '/libpin_auth_interface_service_3.0/,/external_deps/p' $A/BUILD.gn | grep -o -E '"[^"]+\.(c|cpp)"' | tr -d '"'); do
    p=$A/$src
    o=$O/obj/${src//\//_}.o; objs+=($o)
    # Its entry point is wrapped by pin_auth_init.cpp, which runs the setup the donor's driver doesn't.
    d=; [ $src = service/src/pin_auth_interface_service.cpp ] &&
        d=-DPinAuthInterfaceImplGetInstance=OpenHarmonyPinAuthInterfaceImplGetInstance
    if [ ${p##*.} = cpp ]; then $CXX $CF $CPP $d -c $p -o $o; else $CC $CF -std=gnu11 -c $p -o $o; fi
done
$CXX $CF $CPP -c $H/pin_auth_init.cpp -o $O/obj/pin_auth_init.o
objs+=($O/obj/pin_auth_init.o)
$CXX $CF $CPP -shared -fuse-ld=lld -Wl,-z,defs -Wl,-soname,libpin_auth_interface_service_3.0.z.so "${objs[@]}" \
    $L/{libc++.so,libutils.z.so,libhilog.so,libhdf_utils.z.so,libhdi.z.so,libipc_single.z.so,libcrypto_openssl.z.so,libsec_shared.z.so,libbegetutil.z.so,libpin_auth_stub_3.0.z.so} \
    -o $O/libpin_auth_interface_service_3.0.z.so
ls -l $O/libpin_auth_interface_service_3.0.z.so
echo PINAUTH-OK
