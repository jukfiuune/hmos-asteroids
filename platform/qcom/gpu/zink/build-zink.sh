#!/bin/bash
# Builds sheng's GLES path on the build PC: Mesa's Zink (OpenGL ES on Vulkan, here on our Turnip) behind an EGL with
# OpenHarmony's native-window platform, from OpenHarmony's Mesa fork (third_party_mesa3d, 25.0.1, commit ea74a63) with
# sheng's compat/graphics/mesa-gallium-egl.patch (zink/sheng): native-buffer images, dma-buf fence export and import
# (the kernel's DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE; sheng's helper module is only a fallback for older kernels),
# and the window platform fixes. Only Zink and EGL come from this tree: Turnip stays the one from ../mesa.
# The libraries are named as HarmonyOS's EGL wrapper (/system/lib64/libEGL.so) loads a vendor driver:
# libEGL_impl.so, libGLESv1_impl.so, libGLESv2_impl.so, libGLESv3_impl.so (= v2), with libgallium beside them.
# Output: $WORK/out-zink/.
set -e
. "$(dirname "$0")/../../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
N=$HB/ohos/sdk/native
SYS=$N/sysroot
S=$HB/ohos/src
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt
SRC=$WORK/mesa-ohos
B=$WORK/build-zink-ohos
O=$WORK/out-zink
mkdir -p $O/link $O/pkgconfig

if [ ! -d $SRC/.git ]; then
    git init -q $SRC
    git -C $SRC remote add origin https://gitee.com/openharmony/third_party_mesa3d.git
    git -C $SRC fetch -q --depth 1 origin ea74a6339917390657db153cb9792f9ccac9f5ae
    git -C $SRC checkout -q FETCH_HEAD
fi
if ! git -C $SRC diff --quiet; then :; else
    git -C $SRC apply $QCOM/gpu/zink/sheng/mesa-gallium-egl.patch
fi
# Emulated TLS (-femulated-tls, as Turnip): the GL dispatch and context are emutls variables in libgallium. It must
# also export the emutls runtime, or each GLES library links in its own copy, with its own storage, and sees no
# current context (glGetString crashed). And it must stay preemptible in libgallium (its dynamic list, dri.dyn, binds
# everything else locally): the system's libc++.so exports an emutls runtime too, which the GLES libraries bind to
# inside HarmonyOS's EGL wrapper, so libgallium has to use that same one.
grep -q '__emutls_get_address;' $SRC/src/gallium/targets/dri.dyn ||
    sed -i 's|^{$|{\n\t__emutls_get_address;|' $SRC/src/gallium/targets/dri.dyn
grep -q '__emutls_get_address;' $SRC/src/gallium/targets/dri/dri.sym.in ||
    sed -i 's|^\( *\)__emutls_v._mesa_glapi_tls_Dispatch;|&\n\1__emutls_get_address;|' $SRC/src/gallium/targets/dri/dri.sym.in
# Zink doesn't render pre-rotated, but kopper asks the swapchain for preTransform = currentTransform. HarmonyOS's
# swapchain reports the window's transform hint there (270 in landscape), so every buffer was tagged as already
# rotated, and render_service drew web pages (ArkWeb is the main GL client) unrotated in landscape. Identity tells
# the truth: render_service rotates the content, as it does for everything else.
grep -q 'asteroids: identity preTransform' $SRC/src/gallium/drivers/zink/zink_kopper.c ||
    sed -i 's|^   cswap->scci.preTransform = cdt->caps.currentTransform;$|   /* asteroids: identity preTransform (Zink never renders rotated) */\n   cswap->scci.preTransform = (cdt->caps.supportedTransforms \& VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?\n      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cdt->caps.currentTransform;|' \
        $SRC/src/gallium/drivers/zink/zink_kopper.c
# And the size: after a resize (ArkUI sets the window's geometry on rotation) the next acquire is out of date and
# kopper recreated the swapchain at the resource's old size; HarmonyOS's swapchain layer then sets the window's
# geometry from that extent, undoing the resize, so the page stayed portrait, stretched over the landscape window.
# The window's own geometry (currentExtent from the layer) is the size to create at.
grep -q 'asteroids: the window' $SRC/src/gallium/drivers/zink/zink_kopper.c || python3 - $SRC/src/gallium/drivers/zink/zink_kopper.c <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """   case KOPPER_OHOS:
      cswap->scci.imageExtent.width = w;
      cswap->scci.imageExtent.height = h;
      break;"""
new = """   case KOPPER_OHOS:
      /* asteroids: the window's current geometry; w/h are the old size when recreating after a resize, and the
       * swapchain layer sets the window's geometry from this extent */
      if (cdt->caps.currentExtent.width && cdt->caps.currentExtent.height &&
          cdt->caps.currentExtent.width != 0xFFFFFFFF) {
         cswap->scci.imageExtent = cdt->caps.currentExtent;
      } else {
         cswap->scci.imageExtent.width = w;
         cswap->scci.imageExtent.height = h;
      }
      break;"""
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))
PY
# Zink offers dma-buf import (and with it EGL_OHOS_image_native_buffer, how ArkWeb shows hardware-decoded video and
# imports other native buffers) only when ohos_dmabuf_sync_available(), which looks for sheng's /dev/sheng_dmabuf_sync:
# his kernel module for a kernel without dma-buf's sync-file ioctls. Our 6.6 has them (since 5.20), and the helpers
# already try them first and fall back to his device only on ENOTTY; without the device every EGLImage from a native
# buffer failed with EGL_BAD_MATCH and web video stayed black.
grep -q 'asteroids: the standard' $SRC/src/util/ohos_dmabuf_sync.h || python3 - $SRC/src/util/ohos_dmabuf_sync.h <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """static inline bool
ohos_dmabuf_sync_available(void)
{
   int fd = ohos_dmabuf_sync_open();"""
new = """static inline bool
ohos_dmabuf_sync_available(void)
{
   /* asteroids: the standard dma-buf sync-file ioctls (Linux 5.20 and later) need no helper device */
   struct utsname name;
   unsigned major = 0, minor = 0;
   if (!uname(&name) && sscanf(name.release, "%u.%u", &major, &minor) == 2 &&
       (major > 5 || (major == 5 && minor >= 20)))
      return true;
   int fd = ohos_dmabuf_sync_open();"""
assert s.count(old) == 1
s = s.replace(old, new).replace("#include <sys/ioctl.h>\n", "#include <stdio.h>\n#include <sys/ioctl.h>\n#include <sys/utsname.h>\n", 1)
open(p, "w").write(s)
PY
# The native image layout accepts only the usage bits it knows; the video decoder's buffers carry VIDEO_DECODER (bit
# 15), which our allocator lays out like any other linear buffer.
grep -q 'asteroids: VIDEO_DECODER' $SRC/src/egl/drivers/dri2/ohos_native_image_layout.h ||
    sed -i 's|^      (1ULL << 9) \| (1ULL << 10) \| (1ULL << 16) \||      (1ULL << 9) \| (1ULL << 10) \| (1ULL << 15) /* asteroids: VIDEO_DECODER */ \| (1ULL << 16) \||' \
        $SRC/src/egl/drivers/dri2/ohos_native_image_layout.h
mkdir -p $O/include
cp $QCOM/gpu/zink/sheng/sheng_dmabuf_sync.h $O/include/

# libhilog (the donor's) and libsurface (here: the SDK's native window, plus the display headers) for meson.
[ -f $O/link/libhilog.so ] || sudo -n cat $DONOR/system/system/lib64/chipset-pub-sdk/libhilog.so > $O/link/libhilog.so 2>/dev/null ||
    sudo -n cat $DONOR/system/system/lib64/libhilog.so > $O/link/libhilog.so
cat > $O/pkgconfig/libhilog.pc <<EOF
Name: libhilog
Description: HarmonyOS hilog (donor)
Version: 1
Cflags: -I$S/hiviewdfx_hilog/interfaces/native/innerkits/include
Libs: -L$O/link -lhilog
EOF
cat > $O/pkgconfig/libsurface.pc <<EOF
Name: libsurface
Description: OpenHarmony native window (SDK) and display types
Version: 1
Cflags: -I$S/graphic_graphic_surface/surface/include -I$SYS/usr/include/native_window -I$S/drivers_peripheral/display/interfaces/include
Libs: -L$SYS/usr/lib/aarch64-linux-ohos -lnative_window -lnative_buffer
EOF
sed -e "s|@HARMONY_BUILD@|$HB|g" -e "s|^pkg-config = .*|pkg-config = 'pkg-config'|" \
    -e "s|^c_args = \['|c_args = ['-I$O/include', '|" -e "s|^cpp_args = \['|cpp_args = ['-I$O/include', '|" \
    $QCOM/gpu/aarch64-ohos.ini > $O/cross.ini

if [ ! -f $B/build.ninja ]; then
    PKG_CONFIG_LIBDIR=$O/pkgconfig PKG_CONFIG_PATH= meson setup $B $SRC --cross-file $O/cross.ini \
        -Dbuildtype=release -Dplatforms=ohos -Dgallium-drivers=zink -Dvulkan-drivers= \
        -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled \
        -Dllvm=disabled -Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dzstd=disabled \
        -Dzlib=disabled -Dlibunwind=disabled -Dvalgrind=disabled -Dlmsensors=disabled -Dselinux=false \
        -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dperfetto=false -Dtools=[] -Dcpp_rtti=false \
        -Degl-lib-suffix=_impl -Dgles-lib-suffix=_impl
fi
PKG_CONFIG_LIBDIR=$O/pkgconfig ninja -C $B -j10
ls -l $B/src/egl/libEGL_impl.so* $B/src/mapi/es1api/libGLESv1_CM_impl.so* $B/src/mapi/es2api/libGLESv2_impl.so* \
    $B/src/gallium/targets/dri/libgallium*.so 2>/dev/null
