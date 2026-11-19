# Camera and flashlight

## Flashlight (2026-11-14, works)

Control Center's flashlight uses the camera framework's torch, so it goes through camera_service (SA 3008) to a camera
HDI host. Huawei's `flashlight_service` (SA 70000) is unrelated (their cast/link framework).

The camera host (`device/asteroids/camera/camera_host.cpp`, asteroids_camera_hdi, uid camera_host, started at
post-fs-data; `build-camera.sh`) registers ICameraHost 1.2 as `camera_service` in the camera device class (before
2026-11-16 a torch-only host, torch_camera_host.cpp: one rear camera with a flash, which it couldn't open).
camera_service switches the torch with `SetFlashlight_V1_2(level)` (1.2 only: with a 1.0 host SceneBoard got 7400201)
and reads `OnFlashlightStatus_V1_2`. The torch is led:torch_0 and led:torch_3 at 200 mA × level, lit by led:switch_0;
the cfg chowns the three brightness files to camera_host. Headers from hdi-gen (camera 1.0-1.2 in one run); stubs and
proxies are the donor's. Checked: the tile turns the flash on and off. The Glyph LEDs (aw20036) are not touched.

## Camera (2026-11-16: Huawei's Camera app previews, takes photos and records video)

The back and front cameras work in HarmonyOS: Huawei's Camera app shows a live 1440×1080 preview at 30 fps, takes
4080×3072 photos into the gallery (portrait, correct colours), zooms 0.66-10× across the ultrawide, main and tele lenses
(the zoom bar's W, 1×, 2×, 4×), focuses where it's tapped, and records 1080p30 video (HEVC, or H.264 with "High
efficiency format" off) with sound that plays in the gallery, through msm_video's encoder ([media.md](media.md)). The
pieces, end to end:

| Piece | Where | Does |
|---|---|---|
| camserver | `platform/qcom/camera/bridge/camserver.c` | Qualcomm's CamX HAL (HAL3) behind a socket; preview frames as NV21 and JPEGs through shared memory (`protocol.h`) |
| camera root | `platform/qcom/boot/svc/camera.sh` (init service asteroids_camera) | CamX's surroundings at /android/camera: Android's linker namespaces, its own binder context, propd, the service managers, the gralloc allocator, the perf HAL; restarts them all if camserver exits |
| camera host | `device/asteroids/camera/camera_host.cpp` (asteroids_camera_hdi) | HarmonyOS's camera HDI 1.2 in front of camserver; also the torch while no camera is open |
| app profile | `device/asteroids/camera/make-camera-profiles.py` | our `sys_prod/etc/camera/CameraProfiles.json` (sizes, modes, zoom steps for Huawei's app) |
| camtool | `platform/qcom/camera/bridge/camtool.c` (`/asteroids/bin/camtool`) | `camtool list`, `camtool shoot <camera> [out.jpg] [off\|on\|torch]` from M1's shell, without HarmonyOS (CAMTOOL_PREVIEW, CAMTOOL_STILL, CAMTOOL_ZOOM) |

The camera files are in the android partition (`platform/qcom/android/build-android-helpers.sh`: CamX's modules,
components and support libraries, the ICP firmware, CamX's overrides, the closure of
`device/asteroids/android/camera-observed.txt`; not Nothing's effect nodes and their ArcSoft libraries). The compute DSP
runs only while a camera is open.

What it took, beyond the test root's findings (`platform/qcom/camera/test/README.md`):

- Preview: YCbCr_420_888 with the preview's usage, so it stays uncompressed (implementation-defined comes out UBWC).
  Gralloc describes these buffers as NV21 but they hold U first; camserver swaps the chroma.
- Abilities camera_service and the app need (camera_host.cpp's Abilities): the profiles as
  OHOS_ABILITY_STREAM_AVAILABLE_EXTEND_CONFIGURATIONS for modes 0-2 with preview, video and JPEG still streams; the
  35 mm equivalent focal length (the app's setup threw without it); camera type unspecified (the app takes only a
  "default" back camera on a phone).
- The app's sizes, modes and zoom steps come from CameraProfiles.json, not the abilities: the donor's (Pura 70) made it
  ask for 4096×3072 YUV photos and never start the preview. Ours offers JPEG photos at the HAL's sizes, Photo and Video
  only, zoom steps 0.66/1/2/4 (as optical steps too: the zoom bar shows those), and Huawei-only processing off.
- The host is camera HDI 1.3 (the donor's libcamera_stub_1.3 and libcamera_proxy_1.3), for the 1.3 stream operator
  callback: after each photo it sends OnFrameShutterEnd and OnCaptureReady. Without them Huawei's app took one photo
  per session: it waits for captureReady before the next ("camera is not ready or wait save photo",
  NOT_CAPTURE_READY) and keeps the camera switch disabled until frameShutterEnd. camera_service registers a 1.3
  callback only for a 1.3 host and device (a 1.3 call on its 1.2 callback fails with 305, unknown command).
- Huawei's libsurface is called by symbol, not through OpenHarmony's headers' vtables; its plane records have the
  row and pixel strides swapped against the header (the row stride is taken from the buffer). JPEGs go in BLOB buffers
  requested as width×height bytes in one row, as our allocator (`platform/qcom/display/buffer_vdi.cpp`) takes BLOB, with
  dataSize, captureId, imageId and the photo's size in the extra data.
- Zoom from the app's buttons arrives as OHOS_CONTROL_SMOOTH_ZOOM_RATIOS ((ratio×100, ms) steps for the HAL to
  animate); camserver goes from where it is to the last step over the same time. CamX takes zoom as
  ANDROID_CONTROL_ZOOM_RATIO, not the crop region.
- The lenses: camserver's back camera is a group (CAMSERVER_CAMERAS=0+2+3 in camera.sh) of HAL cameras 0 (main, its
  1×), 2 (ultrawide) and 3 (tele), each lens's zoom against the main's from their fields of view (focal length over
  the sensor's diagonal): 0.66 and 1.98. A zoom goes to the narrowest lens whose field of view holds it. Handing over
  opens the next HAL camera while the old one is still open (the HAL takes two at once), gives it the same streams,
  and closes the old one in the background: 115-170 ms, where closing first took 0.85-1.1 s (closing alone is ~0.7 s).
  During a smooth zoom the open lens zooms on digitally while it can, so W to 2× hands over once.
- The HAL's own logical camera (4, of 2, 0 and 3, zoom 0.6-10) would switch without any gap, but every request still
  fails in CHI's multi-camera feature graph ("RealTimeMCX deactivated", no sink for the stream). What it took to get
  that far: NCS on (before, its main lens's realtime pipeline didn't finalize), session parameters with the stream
  configuration, and the stream use case (camera3_stream_t's use_case, which CHI reads as the stream's intent). Its
  policy still finds no stream intent, and Nothing's own app sets a vendor operation mode
  (com.nothing.camera.streamconfig.operation_mode) we don't know the values of.
- Focus: the app sets OHOS_CONTROL_FOCUS_MODE AUTO, then OHOS_CONTROL_AF_REGIONS (a normalized point; camera_service
  takes a point only outside CONTINUOUS_AUTO, so the abilities list AUTO and LOCKED). camserver puts an eighth of the
  image around it in AF and AE regions and starts a scan (AF_TRIGGER) on one request; back in CONTINUOUS_AUTO it
  cancels the scan. The HAL's AF and AE states go back to the app as OHOS_CONTROL_FOCUS_STATE and EXPOSURE_STATE in
  OnResult, with the focus and exposure modes (camera_service reads the state only with them).
- The kernel finds firmware in the boot's mount namespace (M1's), so camera.sh copies the ICP firmware to
  /data/asteroids/camera/firmware (the same /data in both) and puts that first in the firmware path (as the fifth
  entry it wasn't reached). Without it CamX's image processing nodes fail and configure returns -19.
- CamX loads some libraries by paths it builds at run time (libhme, libcamxswprocessalgo, libipebpsstriping,
  libswregistrationalgo and libjpege for the tele's and the multi-frame stills), and some components only while
  streams are configured: the android partition keeps them by name or from camera-observed.txt. To find what's missing
  after a change, run `/data/local/tmp/camtest/logdw /android/camera/dev/socket 5` in init's namespace, restart
  camserver (it opens the log socket when it starts) and read the "dlopen failed" lines; or sample
  /proc/<pid>/maps in the test root (full images) while camtool shoots, and compare.
- A new open while a device is still open (camera_service restarted, or the app died) closes the old one.
- Stabilisation: CamX's non-camera sensor service (NCS) brings it the gyro (and accelerometer) from the sensor hub;
  its FastRPC channel used to block while nothing served the sensor PD, and sscrpcd does now, so CamX's overrides no
  longer turn it off. In a video session camserver marks the preview stream as video (the encoder usage, use case 4)
  and asks for ANDROID_CONTROL_VIDEO_STABILIZATION_MODE on; CHI then runs EIS v3 on it with the gyro, though the HAL
  lists only OFF among its modes. The app's setting (OHOS_CONTROL_VIDEO_STABILIZATION_MODE, abilities OFF and AUTO)
  turns it off. No lens lists optical stabilisation.
- Video: the video stream (intent VIDEO, NV21 1920×1080) goes to the recorder's encoder surface like any repeating
  stream, a copy of the preview frame. Switching the app from Photo to Video makes a new session on the open device,
  which asks for the stream operator again with its own callback; the host kept the first session's, so camera_service
  looked the streams up in the released session ("OnCaptureStarted StreamId: 2 not found"), the app never got its
  preview's first frame and ignored the shutter ("first deq image is not appear, ignore capture"). The stream operator
  now takes each session's callbacks (SetCallbacks). Frame timestamps are CLOCK_BOOTTIME (the recorder compares them
  with its stop time).

Not done:
- Other modes: the app shows none beyond Photo and Video ("No more modes"); Pro, Night, Portrait and the rest need
  either Huawei's processing or manual controls the bridge doesn't pass on yet.
- Preview stabilisation in photo mode (EIS runs only in video sessions); how much EIS steadies a clip hasn't been
  measured, only that it runs on every frame with the gyro.

Open: since the android partition grew with the camera (709 MB), the Android root is ready at ~31 s instead of ~13 s,
and the helpers and the desktop come up ~20 s later. Not explained yet.

### Before (2026-11-14)

Hardware present and probed (2026-11-14): the Spectra camera stack (`camera.ko`, camcc), `/dev/video0`, `/dev/video1`,
`/dev/media0-2`, 25 `v4l-subdev` nodes, CCI, the camera LDOs (wl28681 ×2). Sensors per the device tree: 50 MP main,
50 MP 2x telephoto, 8 MP ultrawide, 32 MP front (to confirm from the DT when this starts).

What it takes: Android's camera HAL is Qualcomm CamX/CHI (camera provider, sensor and tuning binaries, a large
bionic stack with its own HIDL/AIDL interfaces) running in /android behind a bridge to HarmonyOS's camera HDI (1.2,
streams via BufferProducer), like audio and Bluetooth. OpenCamera returns METHOD_NOT_SUPPORTED until then, so camera
apps see one camera they can't open. This is the biggest piece of missing hardware.
