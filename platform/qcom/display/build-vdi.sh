#!/bin/bash
# Builds the display VDIs for HarmonyOS on the build PC, with sheng's compiler flags: the composer
# (libdisplay_composer_vdi_impl.z.so: DRM atomic on /dev/dri/card0, static libdrm) and the allocator
# (libdisplay_buffer_vdi_impl.z.so: linear buffers from /dev/dma_heap/system). Huawei's composer_host and
# allocator_host load them in place of the Kirin ones. Output: $WORK/out-vdi/.
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
N=$HB/ohos/sdk/native
U=$HB/ohos/src
C=$QCOM/display
OUT=$WORK/out-vdi
mkdir -p $OUT
# The donor's libc++ (chipset namespace, which composer_host and allocator_host run in).
LIBCXX=$WORK/ohos-link/libc++.so
[ -f $LIBCXX ] || { sudo -n cp $HB/donor/pura70proplus-7.0.0.107/mnt/system/system/lib64/chipset-sdk-sp/libc++.so $LIBCXX;
                    sudo -n chown $(id -u):$(id -g) $LIBCXX; }

CXX="$N/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -std=c++17 \
    -fno-exceptions -fno-rtti -nostdlib++ -include $COMMON/include/platform_cpp_config.h -fuse-ld=lld"
INC="-I$C -I$C/include/vdi-only -I$C/include -I$C/include/display/buffer \
    -I$U/drivers_hdf_core/interfaces/inner_api/hdi/base -I$U/drivers_peripheral/display/buffer/hdi_service/include \
    -I$U/drivers_interface/display/composer/common/include -I$U/hiviewdfx_hilog/interfaces/native/innerkits/include \
    -I$WORK/ohos-stage/include -I$WORK/ohos-stage/include/libdrm"

$CXX $INC -g -shared -fPIC -Wl,-z,defs -Wl,-soname,libdisplay_composer_vdi_impl.z.so \
    $C/composer_vdi.cpp $C/composer_unsupported.cpp $WORK/ohos-stage/lib/libdrm.a $LIBCXX \
    -o $OUT/libdisplay_composer_vdi_impl.z.so
$CXX $INC -g -DACE5_LINEAR_STRIDE_ALIGNMENT=128 -shared -fPIC -Wl,-z,defs -Wl,-soname,libdisplay_buffer_vdi_impl.z.so \
    $C/buffer_vdi.cpp $LIBCXX -o $OUT/libdisplay_buffer_vdi_impl.z.so
# The anco guest's buffers into HarmonyOS BufferHandles, for anco_service_broker's libdelegator (gralloc_priv.cpp).
# No C++ runtime: it loads into anco_service_broker, outside the chipset namespace the donor libc++ is for.
$CXX $INC -g -fno-threadsafe-statics -shared -fPIC -Wl,-z,defs -Wl,-soname,libgralloc_priv.z.so \
    $C/gralloc_priv.cpp -o $OUT/libgralloc_priv.z.so
ls -l $OUT
echo VDI-OK
