# GPU: Vulkan (Turnip) and GLES (Zink)

Adreno 810 (gen8, chip 0x44010000) on KGSL (Qualcomm's stock msm_kgsl, shmem build). Firmware `gen80300_{sqe.fw,
gmu.bin,zap.mbn}` from the NullDebris blobs.

## Vulkan: Turnip

- Mesa main `0d234946` (2026-11-11) + `turnip-a810-sysmem-ccu.patch` (from the iOS port: on A810 the sysmem caches
  and VPC buffers overflow GMEM and depth tests in sysmem passes read garbage), freedreno/KGSL, cross-built with clang
  against an Alpine aarch64 sysroot (`platform/qcom/gpu/mk-sysroots.sh`). "Adreno (TM) 810", Vulkan 1.4, correct with no TU_DEBUG flags.
- OHOS adaptations: sheng's, applied by `platform/qcom/gpu/port-turnip-ohos.py` (VK_OHOS_native_buffer). It replaces
  `vendor/lib64/passthrough/vulkan.hvgr_v200.so` in the /vendor overlay.
- Our fixes, all in port-turnip-ohos.py:
  - OHOS imports are outside the heap budget (render_service re-imports without freeing; after ~1000 frames every
    allocation failed).
  - One KGSL mapping per dma-buf (render_service had ~8300 mappings of 4 screen buffers).
  - An import's BO is released once only Turnip and KGSL hold the dma-buf (fdinfo count 2); pinned import memory stays
    at 130-170 MB over 25 app cycles. The orphan scan runs once per BO, at most 4 scans/s (it was a 25-100 ms stall in
    vkAllocateMemory). The VkDeviceMemory objects themselves (a few hundred bytes each) still leak.
  - NV21 imports like NV12 with Cb/Cr swapped (Gallery's decoded photos). Odd-sized 4:2:0 can't be imported.
- No P010 (10-bit YUV) format, so no HDR.
- Test: `history/m2-early/vktri.c` (`--drm` puts frames on the panel).

## GLES: Zink

OpenGL ES 3.2 through HarmonyOS's own EGL (`/system/lib64/libEGL.so`): "zink Vulkan 1.4 (Adreno (TM) 810 ...
MESA_TURNIP)". `platform/qcom/gpu/zink/build-zink.sh` builds OpenHarmony's Mesa fork (third_party_mesa3d 25.0.1, `ea74a63`, which has
the OHOS EGL platform) with sheng's mesa-gallium-egl.patch (`platform/qcom/gpu/zink/sheng`), Zink only; Turnip stays ours. Libraries
under the names the EGL wrapper loads from `/vendor/lib64/chipsetsdk` (libEGL_impl, libGLESv1/v2/v3_impl,
libgallium-25.0.1). `native_gles_check` makes an ES 3 pbuffer context and reads a pixel back.

- Emulated TLS: libgallium must export the emutls runtime (dri.sym.in) and keep it preemptible (dri.dyn), or the GLES
  libraries see no context (glGetString crashes or returns NULL).
- Rotation (2026-11-13): kopper asked for preTransform = currentTransform, so every buffer claimed a rotation Zink never
  did and web pages didn't turn; now identity. On resize kopper recreated the swapchain at the old size, undoing
  ArkUI's resize; now at currentExtent. With identity and a 90/270 hint the swapchain layer adds usage bit 63
  (Huawei's "rotated for the display subsystem"), which the buffer VDI now accepts.
- EGLImages from native buffers (2026-11-13): Zink offered dma-buf import only if sheng's `/dev/sheng_dmabuf_sync`
  existed (for kernels without dma-buf sync-file ioctls; ours has them). build-zink.sh reports it available on 5.20+
  and lets the decoder's usage bit through. `native_eglimage_check` imports RGBA, NV12 and decoder NV12: all OK. Videos
  play in the browser.

## Memory

render_service's GPU memory is a pool of large blocks (6-48 MB) that grows and gives back (167-286 MB over 20 min);
its heap levels off near 100 MB. Not a leak (stress 4, 2026-11-13).

## Open

- Turnip's pipeline disk cache for render_service (first Control Center open compiles four pipelines, one 74 ms frame).
- Turnip's rendering mode on A810 (GMEM vs sysmem, UBWC) for GPU-heavy apps ([performance.md](performance.md)).
- KGSL allocation: each 48 MB block costs ~19-40 ms (4K pages cleared and cache-cleaned); a pooled KGSL build would fix it.
- GPU frequency shows "unavailable" in DevInfo: it reads a Huawei interface, not KGSL's devfreq.

## Sources

The two Mesa trees on the PC (`~/harmony-build/m2/mesa`, `mesa-ohos`) are recorded as a base commit plus one diff each,
checked to recreate them exactly (2026-11-15):

- Turnip: Mesa main `0d234946736de1f290f31eba328b1385be949afc` (gitlab.freedesktop.org/mesa/mesa) +
  `platform/qcom/gpu/patches/turnip-asteroids.patch` (the A810 sysmem CCU commit, port-turnip-ohos.py's sections and
  tu_ohos.*).
- Zink and EGL: OpenHarmony's third_party_mesa3d `ea74a6339917390657db153cb9792f9ccac9f5ae` (gitee.com/openharmony) +
  `platform/qcom/gpu/patches/zink-ohos.patch` (sheng's mesa-gallium-egl.patch and our window platform fixes).

Cross file: `platform/qcom/gpu/aarch64-ohos.ini`.
