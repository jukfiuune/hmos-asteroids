# Sensors and auto-rotate

## Sensors (sheng's design, extended)

The sensor hub is on the ADSP (SSC). Apps get accelerometer and gyro (STMicro LSM6DSV), magnetometer (MEMSIC
MMC56x3x), ambient light and proximity (Sensortek STK3A5x) through HarmonyOS's own sensor service. Pressure and hall
are probed; this phone has neither.

- ADSP side: `platform/qcom/boot/svc/sscrpcd.sh` (init service asteroids_sscrpcd) runs Android's `sscrpcd sensorspd` in /android. The DSP's registry needs a writable
  `persist/sensors`: a copy in `/data/asteroids/android/vendor/sensors-persist` is bound over it (on the read-only one
  the registry write crashed the ADSP and, with recovery off, the kernel panicked). ADSP recovery is switched on first.
- HarmonyOS side: sheng's sensor HDI (`platform/qcom/sensors/sheng`, `build-sensors.sh`) in `asteroids_sensor_hdi` (sensor_host's
  uid), registered through the donor's stubs; it sets `bootevent.sheng_sensor_hdi.ready`, on which the donor's
  `sensors` service starts. The HDI is HarmonyOS NEXT's sensor 3.1: `platform/qcom/sensors/idl/v3_1/ISensorInterface.idl` is 3.0
  plus five callback-id methods (codes 17-21, as in the donor's proxy), through hdi-gen.
- Our backend changes: on-change sensors (light, proximity) enabled with request 514 and running once acknowledged;
  events with 1-3+ values; proximity from message 769 as 0 cm (near) / 5 cm (far).
- sheng's parcel fix (Sensor::ReadFromParcel order) is in patch-insns.py.
- Tools: `asteroids_sensor_check` (`platform/qcom/sensors/native_sensor_check.c`, log `/data/local/tmp/sensor-check.txt`): lists
  5 sensors and reads all of them; `platform/qcom/sensors/ssc_probe.c` reads the hub over QMI (`SSC_DEBUG=1` for raw messages).
- The magnetometer read ~157 µT at the desk, about 3× Earth's field (probably the laptop); uncalibrated.

## Auto-rotate (beyond sheng)

SceneBoard's rotation comes from msdp's motion service, which wants `motion_interface_service`. `platform/qcom/motion/`:
`native_motion_hdi_publisher` (asteroids_motion_hdi, uid sensor_host) registers the donor's MotionIfService over our
VDI (`motion_vdi.cpp`, vtable checked against the donor's calls), which turns the accelerometer into
HDF_MOTION_TYPE_ROTATION events (status 0..3). Subscribing and all reports run on a worker thread: inside EnableMotion
they hit the service's 8 s watchdog and deadlocked foundation's sensor queue (the UI restarted). msdp's AR retries
are patched to one try (it restarted every 10-20 s and dropped the subscription). `asteroids.motion.force_rotation` is
a test hook. The user saw the screen rotate; apps pick their own orientation (Settings portrait, web pages free).

## Open

- Not yet seen through the UI: auto-brightness, a compass (the Compass app isn't installed), the screen going off near
  the ear in calls (proximity).
- Step counter, significant motion, pick-up/flip gestures (msdp's other HDIs, the AR HDI): not provided.
- Magnetometer calibration.
