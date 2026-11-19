# Vibration

LRA on a Richtek/ICSense RT6010 (hapticdrv, I2C 8-005f, f0 ~174 Hz from the bootloader's `haptic_f0`). Played the way
Nothing's Android and LineageOS/EvoX do it (NullDebris' `hardware/nothing/vibrator`, nothing-rt_ics): through AAC's
closed RichTap engine, `libaacvibrator` (vendor), with `/odm/etc/aac_richtap.config`. The user found the result
"perfect" (2026-11-14); our own waveforms before it were buzzy, then sharp.

## Kernel

The driver (NullDebris, `drivers/misc/haptic`) offers the input device haptic_rt (FF_CONSTANT plays the chip's RAM
waveform for N ms) and the RichTap device `/dev/aac_richtap`. It asks for `haptic_config.bin` once, 8 s after probe,
when only M1's root exists, so the chip was never initialised (no trim, RAM layout, FIFO, boost, gain, f0).
`device/asteroids/vibrator/hapticdrv-chip-init.patch` retries every 2 s for 3 minutes, starts that only once the probe can't fail and
cleans up on remove. The rebuilt module is the one in vendor_boot (since 2026-11-15; before that prep.sh swapped it
in at boot). Firmware: `haptic_config.bin`, `100_Haptic.bin`, `101_Haptic.bin` (Android's, copied from the stock vendor image into `$HARMONY_BUILD/fw/vibrator/` by `device/asteroids/vibrator/extract-firmware.sh`,
in vendor_boot's `/vendor/firmware` so the first request finds them, and in our `/vendor/firmware`). The driver's sysfs files stay compiled out, as in EvoX.

## RichTap server

`device/asteroids/vibrator/richtap_server.c` (bionic): agmplay under Android's linker in /android with its `__libc_init` taken over,
loads libaacvibrator with nothing-rt_ics's settings (`RICHTAP_DEVICE_PATH=/dev/aac_richtap`,
`ENV_RICHTAP_CONFIG_PATH`), runs its init and looper, and serves on `/dev/unix/socket/asteroids.richtap` (root:3080
0660): on(ms), effect (12296 click, 12297 tick, strength 100, amplitude 0..255), stop. `device/asteroids/boot/svc/richtap.sh` (init
service asteroids_richtap). libics_haptic (f0 tracking) is left out: it needs the sysfs files.

## HarmonyOS side

misc device service (SA 3602, in the sensors process) → vibrator_interface_service. `native_vibrator_hdi_publisher`
(asteroids_vibrator_hdi, uid vibrator_host) registers the donor's libvibrator_interface_service_2.0 over our VDI
(`vibrator_vdi.cpp`, vtable as drivers_peripheral's, checked against Huawei's 37 slots). Mapping as nothing-rt_ics maps
Android's: one-shots and continuous events → on; presets → click (tick for clock, slide, threshold, soft, light;
notices 40 ms on); composite primitives → clicks; intensity → amplitude with the 1.2 gamma. Until the server is up,
pulses go to the RAM sine through force feedback (`haptic_ff.c`).

Test from a shell in the HarmonyOS root: `asteroids_vibrate <ms> | click | tick [amplitude]`.

## Open

- f0 tracking (libics_haptic needs the driver's sysfs files).
- HD haptics patterns from apps beyond presets: mapped to clicks/on, not the engine's HE playback.
