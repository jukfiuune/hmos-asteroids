#!/bin/bash
# Builds sheng's Bluetooth path (compat/bluetooth, copied to bluetooth/sheng) on the build PC.
#   Android side (Bionic, run under Android's linker64 in /android by start-bluetooth.sh): libsheng_bt_bridge.so takes
#     over agmplay's __libc_init, loads Qualcomm's HAL (android.hardware.bluetooth@1.0-impl-qti.so), which powers the
#     WCN chip through /dev/btpower and talks H4 over /dev/ttyHS0, and relays HCI packets over the unix socket
#     sheng.bt.h4. It passes std::function to the HAL, so it's compiled against libc++ headers with Android's ABI
#     namespace (__1: the SDK's host headers; the OHOS ones use __n1) and linked to the phone's Android libc++.
#   HarmonyOS side (OHOS SDK): libbt_vendor_sheng.so, the vendor library the donor's HCI service loads (bound over
#     /vendor/lib64/libbt_vendor.z.so), which connects to that socket; native_bt_hdi_publisher hosts the donor's HCI
#     implementation and registers it as hci_interface_service (the donor's bluetooth_host isn't started).
# Output: $WORK/out-bluetooth/.
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
B=$QCOM/bluetooth/sheng
N=$HB/ohos/sdk/native
S=$HB/ohos/src
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt
O=$WORK/out-bluetooth
L=$O/link
mkdir -p $L
for f in system/system/lib64/chipset-sdk-sp/{libc++.so,libutils.z.so,libhdi.z.so,libhdi_base.z.so} \
         system/system/lib64/chipset-pub-sdk/{libipc_single.z.so,libipc_common.z.so} \
         vendor/lib64/libbluetooth_hci_stub_1.0.z.so; do
    [ -f $L/$(basename $f) ] || sudo -n cat $DONOR/$f > $L/$(basename $f)
done
HDF=$S/drivers_hdf_core/interfaces/inner_api
INC="-I$B -I$B/include -I$B/include/bluetooth/hci -I$HDF/hdi -I$HDF/ipc -I$HDF/hdi/base -I$HDF/utils -I$HDF/osal/uhdf
     -I$HDF/core -I$S/commonlibrary_c_utils/base/include -I$S/communication_ipc/interfaces/innerkits/ipc_core/include
     -I$S/communication_ipc/interfaces/innerkits/ipc_single/include -I$S/hiviewdfx_hilog/interfaces/native/innerkits/include"
CXX="$N/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -std=c++17
     -fno-exceptions -fno-rtti -nostdlib++ -include $COMMON/include/platform_cpp_config.h -fuse-ld=lld"
$CXX $INC $B/native_bt_hdi_publisher_v136.cpp $L/libbluetooth_hci_stub_1.0.z.so $L/libhdi.z.so $L/libhdi_base.z.so \
    $L/libutils.z.so $L/libipc_single.z.so $L/libipc_common.z.so $L/libc++.so -ldl -Wl,--allow-shlib-undefined \
    -o $O/native_bt_hdi_publisher
$N/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld \
    -I$S/drivers_peripheral/bluetooth/hci/hdi_service/implement -shared -fPIC $B/bluetooth_vendor_v136.cpp -ldl \
    -o $O/libbt_vendor_sheng.so
# A test client for the bridge (root, beside HarmonyOS; static).
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -fuse-ld=lld -static \
    $QCOM/bluetooth/bt_hci_test.c -o $O/bt_hci_test
H=$N/llvm/include
$N/llvm/bin/clang++ --target=aarch64-linux-android33 --sysroot=$N/sysroot -isystem $N/sysroot/usr/include/aarch64-linux-ohos \
    -nostdinc++ -isystem $H/c++/v1 -isystem $H/x86_64-unknown-linux-gnu/c++/v1 -D_LIBCPP_HAS_THREAD_API_PTHREAD \
    -fuse-ld=lld -fPIC -shared -nostdlib -D_GNU_SOURCE -mno-outline-atomics -O2 -Wall -Wextra -Wl,-z,defs \
    -std=c++17 -fno-exceptions -L$WORK/bionic-link $B/bluetooth_bridge_v136.cpp -lc++ -lc -ldl \
    -o $O/libsheng_bt_bridge.so
ls -l $O/*.so $O/native_* $O/bt_hci_test
