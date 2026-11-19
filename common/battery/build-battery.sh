#!/bin/bash
# Builds sheng's battery HDI publisher (compat/battery) on the build PC. It hosts the donor's own battery
# implementation (libbattery_interface_service_2.0, reading /sys/class/power_supply) and registers it with the HDF
# service manager as battery_interface_service, which powermgr's battery service looks for; the donor's power_host,
# which would normally host it, isn't started. native_battery_probe reads the implementation directly, for testing.
# Output: $WORK/out-battery/.
set -e
. "$(dirname "$0")/../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
B=$COMMON/battery
N=$HB/ohos/sdk/native
S=$HB/ohos/src
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt
O=$WORK/out-battery
L=$O/link
mkdir -p $L
for f in system/system/lib64/chipset-sdk-sp/{libc++.so,libutils.z.so,libhdi.z.so,libhdi_base.z.so} \
         system/system/lib64/chipset-pub-sdk/{libipc_single.z.so,libipc_common.z.so}; do
    [ -f $L/$(basename $f) ] || sudo -n cat $DONOR/$f > $L/$(basename $f)
done
HDF=$S/drivers_hdf_core/interfaces/inner_api
INC="-I$B -I$B/include -I$B/include/battery -I$HDF/hdi -I$HDF/ipc -I$HDF/hdi/base -I$HDF/utils -I$HDF/osal/uhdf
     -I$HDF/core -I$S/commonlibrary_c_utils/base/include -I$S/communication_ipc/interfaces/innerkits/ipc_core/include
     -I$S/communication_ipc/interfaces/innerkits/ipc_single/include -I$S/hiviewdfx_hilog/interfaces/native/innerkits/include"
CXX="$N/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -std=c++17
     -fno-exceptions -fno-rtti -nostdlib++ -include $COMMON/include/platform_cpp_config.h -fuse-ld=lld"
LIBS="$L/libhdi.z.so $L/libhdi_base.z.so $L/libutils.z.so $L/libipc_single.z.so $L/libipc_common.z.so $L/libc++.so
      -ldl -Wl,--allow-shlib-undefined"
$CXX $INC $B/native_battery_hdi_publisher_v131.cpp $B/battery_charge_adapter.cpp $LIBS -o $O/native_battery_hdi_publisher
$CXX $INC $B/native_battery_probe_v131.cpp $LIBS -o $O/native_battery_probe
ls -l $O/native_battery_*
