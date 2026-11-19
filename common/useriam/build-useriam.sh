#!/bin/bash
# Builds sheng's software user-auth HDI for the donor's user_auth_host (build PC). No TEE: credentials live in
# software, so useriam can answer "no PIN enrolled" and the lock screen can unlock. Follows sheng's
# tools/build_useriam.py and compat/useriam/build_backend.py:
#   libsheng_userauth_4_0.so                 OpenHarmony's user_auth software backend (hdi_service sources, with
#                                            sheng's user_auth_interface_service.cpp)
#   libuser_auth_interface_service_4.1.z.so  sheng's 4.1 adapter over it (replaces the donor's TEE-backed one)
#   libuser_auth_driver.z.so                 sheng's HDF driver (the donor HCB's user_auth_host loads it by this name)
# Output: $WORK/out-useriam/.
set -e
. "$(dirname "$0")/../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
B=$COMMON/useriam
N=$HB/ohos/sdk/native
S=$HB/ohos/src
A=$S/drivers_peripheral/user_auth/hdi_service
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt
O=$WORK/out-useriam
L=$O/link
mkdir -p $L
for f in system/system/lib64/chipset-sdk-sp/{libc++.so,libutils.z.so,libhdf_utils.z.so,libhdi.z.so,libcrypto_openssl.z.so,libsec_shared.z.so,libhdf_host.z.so,libhdf_ipc_adapter.z.so} \
         system/system/lib64/chipset-pub-sdk/{libhilog.so,libipc_single.z.so} \
         vendor/lib64/{libuser_auth_stub_4.0.z.so,libuser_auth_stub_4.1.z.so}; do
    [ -f $L/$(basename $f) ] || sudo -n cat $DONOR/$f > $L/$(basename $f)
done
HDF=$S/drivers_hdf_core/interfaces/inner_api
mkdir -p $O/openssl-include && cp -r /usr/include/openssl $O/openssl-include/
# OpenSSL: the donor ships 3.0.9; the host's 3.x headers are API-compatible for the EVP/HMAC/RAND calls used here.
INC="-I$O/openssl-include -I$B/generated -I$B/generated/user_auth -I$A/interface -I$A/adaptor/inc -I$A/coauth/inc
     -I$A/common/inc -I$A/database/inc -I$A/idm/inc -I$A/key_mgr/inc -I$A/user_auth/inc -I$A/utils/inc -I$A/service
     -I$A/../common/logs -I$A/../common/utils -I$S/third_party_bounds_checking_function/include
     -I$S/hiviewdfx_hilog/interfaces/native/innerkits/include -I$S/commonlibrary_c_utils/base/include
     -I$S/communication_ipc/interfaces/innerkits/ipc_core/include -I$HDF/utils -I$HDF/hdi -I$HDF/hdi/base
     -I$HDF/host/shared -I$HDF/core -I$HDF/ipc -I$HDF/osal/uhdf -I$HDF/osal/shared"
CF="--target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -fPIC -Wall -Wextra -Wno-unused-parameter
        -Wno-deprecated-declarations $INC"
CPP="-fno-rtti -std=c++17 -nostdlib++ -include $COMMON/include/platform_cpp_config.h"
CC=$N/llvm/bin/clang; CXX=$N/llvm/bin/clang++
mkdir -p $O/obj
objs=()
for src in $(sed -n '1,/external_deps/p' $A/BUILD.gn | grep -o -E '"[^"]+\.(c|cpp)"' | tr -d '"'); do
    p=$A/$src; [ $src = service/user_auth_interface_service.cpp ] && p=$B/user_auth_interface_service.cpp
    o=$O/obj/${src//\//_}.o; objs+=($o)
    if [ ${p##*.} = cpp ]; then $CXX $CF $CPP -c $p -o $o; else $CC $CF -std=gnu11 -c $p -o $o; fi
done
$CXX $CF $CPP -shared -fuse-ld=lld -Wl,-z,defs -Wl,-soname,libsheng_userauth_4_0.so "${objs[@]}" \
    $L/{libc++.so,libutils.z.so,libhilog.so,libhdf_utils.z.so,libhdi.z.so,libipc_single.z.so,libcrypto_openssl.z.so,libsec_shared.z.so,libuser_auth_stub_4.0.z.so} \
    -o $O/libsheng_userauth_4_0.so
INC41="-I$B/generated41 -I$B/generated41/user_auth"
$CXX $CF $INC41 $CPP -shared -fuse-ld=lld -Wl,-z,defs -Wl,-soname,libuser_auth_interface_service_4.1.z.so \
    $B/adapter41.cpp $L/libc++.so $L/libutils.z.so -ldl -o $O/libuser_auth_interface_service_4.1.z.so
# Userspace HDF first: the kernel-side hdf_device_section.h puts the entry in a section instead of exporting driverDesc.
HDFINC="-I$HDF/host/uhdf "$(for h in hdf_device_desc.h hdf_sbuf_ipc.h hdf_object_manager.h hdf_device_section.h; do
    find $S/drivers_hdf_core -name $h -printf '-I%h\n'; done | sort -u | tr '\n' ' ')
$CXX $CF $INC41 $HDFINC $CPP -D__USER__ -shared -fuse-ld=lld -Wl,-z,defs -Wl,-soname,libuser_auth_driver.z.so \
    $B/driver41.cpp $O/libuser_auth_interface_service_4.1.z.so \
    $L/{libc++.so,libutils.z.so,libhilog.so,libhdf_utils.z.so,libhdi.z.so,libipc_single.z.so,libuser_auth_stub_4.0.z.so,libuser_auth_stub_4.1.z.so,libhdf_host.z.so,libhdf_ipc_adapter.z.so} \
    -ldl -Wl,--allow-shlib-undefined -o $O/libuser_auth_driver.z.so
ls -l $O/*.so
echo USERIAM-OK
