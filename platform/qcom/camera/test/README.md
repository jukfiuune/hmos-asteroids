# CamX test root

Loads Qualcomm's camera HAL (CamX, `vendor/lib64/hw/camera.qcom.so`) on the phone the way Android's camera provider
would, outside HarmonyOS, to find out what it needs before it gets a bridge to HarmonyOS's camera HDI.

It runs in `/camroot`, set up from M1's shell. That root has:

- Android's full slot b system, vendor and odm images;
- the runtime and VNDK 34 APEXes from `/android`;
- its own `/dev`, with a new binderfs instance, so it gets an Android binder context separate from HarmonyOS's.

Around the HAL it runs the services Android's camera provider would have:

| Service | What it is |
|---|---|
| `propd` | property area and property service (`platform/qcom/android/propd.c`) |
| `servicemanager`, `hwservicemanager` | AIDL and HIDL service managers |
| QTI gralloc allocator | CamX allocates its buffers through it |
| QTI perf HAL (`IQspmhal`) | CamX waits for it |

Logs go to `logdw` (`platform/qcom/android/logdw.c`).

## Inputs on the phone (`/data/local/tmp/camtest`)

- `system.img`, `vendor.img`, `odm.img`: on the PC, `android-b/system_b.img`, `vendor_b.img` and `odm_b.img`.
- `logdw`, `system_ext.tgz`: from `../build-camera.sh`.
- `m2cam/libpropd.so`, `m2cam/libcamx_probe.so`: from `../build-camera.sh`.
- `camx-test.sh`

```
./port sh 'sh /data/local/tmp/camtest/camx-test.sh root'
./port sh 'sh /data/local/tmp/camtest/camx-test.sh stack'
./port sh 'sh /data/local/tmp/camtest/camx-test.sh probe' 150
./port sh 'sh /data/local/tmp/camtest/camx-test.sh capture 0' 200
./port get /data/local/tmp/camtest/data/capture.jpg out/capture.jpg
./port sh 'sh /data/local/tmp/camtest/camx-test.sh stop'
```

`stop` undoes the root: its processes, its mounts, the firmware path, the CDSP. Left up, the stack ran for hours and
kept the CDSP on.

`capture` takes a 4080×3072 JPEG with the main camera, the flash on as a torch (`CAMX_TORCH=` turns it off): 40
preview frames at 1920×1080 so auto exposure settles, then one request with the JPEG stream. `stacks <pid>` prints
every thread's backtrace of a hung process (through tombstoned and `debuggerd -b`).

## What it took

- **ICP firmware.** The kernel only finds `CAMERA_ICP` when its directory is on `firmware_class.path`. The kernel
  resolves that path in init's root, so the firmware is copied to `camtest/fw`.
- **A property area.** Without one, libbinder waits forever for `servicemanager.ready`.
- **A property service.** hwservicemanager exits if it can't set `hwservicemanager.disabled`.
- **The VINTF SKU.** servicemanager only accepts the allocator once `ro.boot.product.vendor.sku=volcano` is set, since
  the device manifest is `manifest_volcano.xml`.
- **hwservicemanager**, from system_ext. CamX's QTI mapper is a HIDL passthrough interface.
- **The HMI layout.** This HAL's `camera_module_t` has the two torch-strength calls before `init`.
- **`enableNCSService=FALSE`** (until 2026-11-16). The non-camera sensor service's FastRPC channel to the sensor PD
  blocked in `set_callbacks` while nothing served the sensor PD. With sscrpcd running (as the boot does now) it
  connects and EIS gets the gyro, so the override is gone.
- **`vendor/lib64/hw` on the library path.** It holds Nothing's `libproviderextension.so`, which the CHI override
  needs.

For the capture:

- **The probe on its own thread.** A preload's constructor runs under the linker's lock, and CamX's pipeline threads
  dlopen and dlsym: run from the constructor, building the pipeline deadlocked. The host is `vendor/bin/sleep`.
- **Android's linker namespaces.** In one flat namespace the vendor's allocator interface V1 met libui's V2 (for
  AHardwareBuffer). The root runs Android's own `linkerconfig` (with an `apex-info-list.xml` for the two APEXes), and
  the probe runs as a vendor program from the vendor overlay, where the vendor namespace may load it.
- **This HAL's structures.** `camera3_device_ops` has `construct_default_physical_request_settings` after
  `construct_default_request_settings`, so `process_capture_request` and `flush` sit one slot later than AOSP's.
  `camera3_stream_buffer_t` is 40 bytes, not 32 (the stride in CHI's `Usecase::PreprocessCaptureRequest`), and
  `camera3_stream_t` has a group id after `physical_camera_id`, -1 for none. Before these were right, CamX imported
  garbage buffers and crashed in a CFI check.
- **The CDSP.** The boot leaves the compute DSP offline; CamX's motion estimation (`libhme`) runs there and crashed
  the pipeline without it. `root` starts it.
- **Buffers in the requests and `halBufferMgrMode=0`.** CHI's stream buffer manager asks for buffers through
  `request_stream_buffers` on top of the request's and keeps one per frame, so any pool runs dry after ~20 frames; with
  it off the buffers in the requests are enough. The probe still implements the callback.
- **A camera app's preview stream.** A CPU-read YUV stream counts as a snapshot stream to CHI; implementation-defined
  with GPU texture and composer usage is the preview.
- **`overrideEnableMFNR=FALSE`.** In low light the JPEG went through multi-frame noise reduction, which wants a queue
  of earlier full-size frames.

Result:

- the kernel probes all four sensors: s5kgn9 main, s5kkd1 front, imx355 ultrawide, s5kjn5 tele;
- the HAL lists five cameras: those four plus a logical multi-camera;
- every camera reports device version 3.7 and its static characteristics;
- `capture 0` gives a sharp, correctly exposed 12 MP JPEG with no frame errors (2026-11-16).
