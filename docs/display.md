# Display

Panel: Visionox RM69220 on DSI-1, 1080x2392, 120/90/60 Hz, brightness 0-4095 (`panel0-backlight`). msm_drm also
reports a "connected" 5120x2560 virtual writeback connector, so pick DSI explicitly. The panel stays black after a
modeset until `bl_power` is 0 (it was 4, powered down); Android's display HAL does this, here the composer VDI does.

## Path

bootanimation / apps → render_service (Skia Vulkan on Turnip, [gpu.md](gpu.md)) → Huawei's composer_host with our DRM
composer VDI → panel. Allocator: Huawei's allocator_host with our buffer VDI.

- VDIs: sheng's (`platform/qcom/display`; the PC builds from `compat/display`, a copy: change both), changed for one DSI panel:
  any even width, one full-width plane (sheng's left/right split squeezed ours), `bl_power=0` when the backlight goes
  on. render_service needs `persist.rosen.disableddgr.enabled=1` (DDGR wants Huawei-only extensions).
- Direct scanout of one full-screen layer is the default (`SHENG_DEFAULT_DISPLAY_ROUTE 2`). Before it, render_service
  (told every layer is client composition) redrew its layer into the framebuffer every frame and imported the buffer
  afresh each time (~9500 imports in minutes, 15-28 fps after a while), and a "black shape" was the framebuffer
  scanned out while still drawn into. Multi-layer frames still take the client path, where the shape could appear.
- Commits are `DRM_MODE_ATOMIC_NONBLOCK` with the GPU fence as the plane's input fence (blocking fallback on EBUSY,
  never seen); power-on and test commits block.
- Present-fence leak (sheng's present-fence-export, found again): Huawei's `libdisplay_composer_service_1.5` dups each
  commit's present fence and then `Move()`s the original without closing it: +100 fds/s while the screen moved
  (limit 32768). `donor/pura70proplus-7.0.0.107/patches/patch-insns.py` NOPs the two Moves after `replyCommandCnt_++`; the library is in the
  /vendor overlay. 26 fds before and after a burst of swipes.
- composer_host's own stats: `/data/local/tmp/sheng-composer-metrics.txt` (mode 666), FRAME_GAPS.

## Android container windows (2026-11-17)

The anco Android guest ([apps.md](apps.md)) draws with Nothing's Adreno driver and QTI gralloc. anco_service_broker's
libdelegator turns each buffer the guest queues into a BufferHandle for the app's HarmonyOS window by calling
`convert_buffer_handle` in `/vendor/lib64/passthrough/libgralloc_priv.z.so` (Kirin's reads Kirin's gralloc handle). Ours,
`platform/qcom/display/gralloc_priv.cpp`, reads QTI's `private_handle_t`. It takes only linear RGB: the guest's UBWC is
off. It hands over the buffer fd, the visible size, Adreno's pitch, and HarmonyOS's format and usage, with no reserved
fds or ints. The buffer VDI's checks also take Adreno's pitch: a wider row aligned to 64 bytes, without our extra backing
rows. Checked: DroiTong's app store page appeared in its window (1080x2392, stride 4352, RGBA_8888). Both log under
ACE5_DISPLAY: the first conversions, every refused handle, and every buffer RegisterBuffer refuses.

## Configuration

- `device/asteroids/display/display_manager_config.xml` (bound over sys_prod's by prep.sh): the camera hole from the device tree's
  `config_mainBuiltInDisplayCutout` (path `M519 47 L562 47 v 43 h -43 Z`, a 43 px square around the punch hole) and
  460 dpi (the donor's 376 vp width). `persist.window.screen.dpi` was 540 and was set to 460 once by hand (persisted).
- Refresh rate setting (as sheng's patch_refresh_policy.py): prep.sh adds `const.settings.refresh_rate_seenable=true`
  and binds a policy where 120 Hz is fixed 120 (strategy 12) without LTPO (`device/asteroids/display/patch-hgm-policy.py`). Settings
  > Display & brightness > Screen refresh rate: Dynamic idles at 60, High holds 120, Standard 60.
- HDR: off. The donor's `const.display.hdr_picture_support` and the AI-HDR switches made Gallery decode into 10-bit
  P010, which Turnip has no format for; prep.sh turns the three off, and `vendor/etc/param/asteroids-graphic.para` sets
  `persist.sys.graphic.hdrimage.enabled` and `hdrvideo.enabled` to 0. The panel has HDR; we use none of it.
- Photos (2026-11-12): the buffer VDI's `GetImageLayout` didn't follow HarmonyOS's convention, and Gallery's decoder
  wrote the chroma elsewhere (green photos). HarmonyOS reads hStride as the sample step and vStride as the row stride
  (`native_image.cpp`), and YUV always as Y, U, V planes whatever the memory order (NV21's plane 2 starts the
  interleaved VU, plane 1 one byte later). Fixed in `platform/qcom/display/buffer_vdi.cpp`; the allocator also accepts usage
  bit 63 ([gpu.md](gpu.md)) and makes NV21, but refuses odd-sized NV21.
- Manual brightness: the composer applied and read back 1000 levels. Auto-brightness: not yet seen working through
  the UI (the light sensor reads, [sensors.md](sensors.md)).
- Screen off powers the panel down (DPMS off, backlight 0); the power key wakes it.

## Boot animation

Picture mode with frames from the donor's boot video (`donor/pura70proplus-7.0.0.107/bootpic`, `make-bootpic.py`), sheng's Vulkan shim
(`libsheng_boot_vulkan.so`, GPU context member at +328 in this donor) through a launcher, and the donor binary patched
to draw full screen (`donor/pura70proplus-7.0.0.107/patches/patch-bootanimation-fullpic.py`). With direct scanout render_service stopped
presenting it after a few frames, so svc/route.sh writes route 0 to `/dev/sheng-display-route-mode` as soon as `/dev`
is up and removes it at desktop up. Why direct scanout stalls it is not known. The first frame takes ~6 s (no shader
cache). `chipset_vsync_service` is missing (software vsync).

## Lock screen and AOD

- After boot (2026-11-12), SceneBoard (which hosts the window manager) died a few seconds after the first screen-off
  and restarted, and its new instance showed the "fake AOD" view (OneMirrorService "not support aod") at full
  brightness, the hour's first digit missing. Not reproduced in three reboots after that day's fixes (memmgr, msdp,
  the motion deadlock), but not closed: init panics the phone when SceneBoard exits 4 times in 240 s, which happened
  once during injected wake/power presses on that view. Watch init's "SceneBoard exits N times" and SceneBoard's pid.
- aod_service is in the service set; AOD itself isn't supported here.
