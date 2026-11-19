# HarmonyOS framework: services, patches, identity

Donor: Huawei Pura 70 Pro+ (HBN-AL10), HarmonyOS 7.0.0.107, `update_full_base.zip` from Huawei's CDN unpacked with
the PC's `~/harmony-build/tools/unpack_huawei_package` (into `~/harmony-build/donor/pura70proplus-7.0.0.107`). Its system, vendor, sys_prod,
chip_prod and cust are used unmodified (now in super, [storage.md](storage.md)); everything we change goes into
overlays and bind mounts made by the M2 bundle.

## The M2 bundle

`build/build-asteroids.sh` (build PC; `./port build asteroids`) makes `asteroids.img`, the asteroids partition in super,
which M1 mounts read-only at `/asteroids`. Its layout and what runs from it: [boot.md](boot.md#the-asteroids-partition).
What it does to the root before the handoff is `common/boot/prep.sh` and its steps (`prep.d/`): the /vendor overlay (our
VDIs, libraries, firmware), configs bound over the donor's (made at build time by `build/build-etc.sh` from the donor's
own, byte-identical to what the old prep.sh made at every boot), the Base/Pro name, serial and chip name, Wi-Fi and
modem bring-up, the protection-domain mapper. Everything after the handoff (the Android helpers, cgroups, clock,
desktop-up work) is HarmonyOS init services (`common/boot/init/asteroids-common.cfg`). An update is `./port build asteroids`
and `./port deploy asteroids`. prep.sh sources `/data/asteroids/m2-test.sh` if present, for experiments without a
rebuild.

## Service set

- M1's base set plus `donor/pura70proplus-7.0.0.107/desktop-services.txt` (the donor framework services the desktop needs, and the ones added
  one by one: bluetooth, camera, aod, missing-service finds) plus `donor/pura70proplus-7.0.0.107/ondemand-services.txt` (all 170 on-demand
  configs). 221 configs are installed (checked 2026-11-14). On 2026-11-12 installing all 170 broke foundation's access
  token (InitHapToken denied); with today's set there are no InitHapToken failures in hilog.
- `scripts/missing-services.sh` lists the system abilities processes asked samgr for and didn't get ("NF SA:"),
  most-asked first. From it (2026-11-13): CollaborationFwk, resource_schedule_executor, huawei_id_svc,
  display_engine_service. Left out: cadaemon (8001, Huawei's TEE client; this phone has Qualcomm's TEE),
  satellite_service. Lookups of missing services fell from 634 in five minutes to 108 in the first minute.
- Huawei's Android container set (hmos_fusion_manager, rgm_manager, rgm_engine_plugin, iSulad, virt_device_broker,
  broker, bms_broker) is still in desktop-services.txt; isulad runs from boot for nothing ([apps.md](apps.md)). Drop the
  line to keep boot lean.

## Binary patches (`donor/pura70proplus-7.0.0.107/patches/patch-insns.py`)

sheng's client patches found again in our donor, and ours. Each is a byte patch at a checked location:

| Library | Patch |
|---|---|
| SceneBoard's clients | don't block the UI thread on services that can't come up (sheng) |
| libpowermgrservice | input-activity interval 0 (with 1000 ms nothing arrived and the screen dimmed under the finger) |
| libdisplay_composer_service_1.5 | present-fence Move NOPs ([display.md](display.md)) |
| libtime_system_ability | clock-12 timerfd failure no longer abandons the 7-timer setup (sheng) |
| libsensor_utils | Sensor::ReadFromParcel order (sheng) |
| memmgr | ignore ReclaimStrategyManager::Init's failure (no Huawei memcg files), so SA 1909 publishes and the soft keyboard stays up |
| libmovement_ar (msdp) | try the AR HDI once (20 retries × per type blocked past msdp's 5 s watchdog, msdp restarted every 10-20 s) |
| libhmos_fusion_manager | caller checks return 0 ([apps.md](apps.md)) |
| (rs-commit-sync, sheng) | still in the script, not applied: the kernel fix replaced it |

Also: sheng's no-TEE key shims, code-sign patch (fs-verity skip, `patch-code-sign.py`) and fallbacks in `common/framework/`;
`patch-key-enable.py` ([apps.md](apps.md)).

## Software replacements for Huawei's TEE

- HUKS: huks_service never published SA 3510 (its HDI is an iTrustee client). `common/framework/build-huks-engine.sh`
  builds OpenHarmony's software key engine (OpenSSL backend, root key in `/data/data/huks_service`) under the HDI's
  soname. Apps' key operations work (Weather imports its keys); key attestation can't.
- User auth: sheng's software useriam (`common/useriam/`, needs `ohos.boot.sn`, written by prep.sh from the Android
  serial) with our 4.1 adapter. PIN: [lockscreen.md](lockscreen.md).

## Device identity

- Name: prep.sh writes "Nothing Phone (3a)" / A059 (or "(3a) Pro" / A059P) into the hw_defaults.para it binds, by
  `androidboot.pbid` (Base/Pro) or else the device tree's model. This phone: Base, sku EEA. Brand and manufacturer stay
  HUAWEI (Huawei's services check them). Settings and About show the name and model A059.
- Chip: apps' `deviceInfo.chipType` is `ohos.boot.hardware`, "asteroids" from M1 (no Kirin configs load); prep.sh
  rewrites it to "Snapdragon 7s Gen 3" in the cmdline and bootconfig M1 binds over /proc. Init takes the value up to
  the first space and ignores quotes, so the words are joined by no-break spaces. Device-info apps show it; Settings
  doesn't show a chip.
- Serial: `ohos.boot.sn` from the Android serial number.

## Other framework fixes

- Time: network time syncs over Wi-Fi (checked by setting the clock back 5 minutes and toggling automatic time).
- Telephony, location, power HDI, vibrator, sensors: see their files.
- `asteroids_dump` (init service with ohos.permission.DUMP) runs `/data/local/tmp/asteroids-dump.sh`: hidumper refuses a
  root shell. `hidumper_service.cfg` is installed.

## sheng's pieces we don't use

- His storage_daemon preload (`storage_ram_no_tee.c`): user 100's storage stopped mounting with it.
- His memmgr meminfo patch: our donor already falls back to /proc/meminfo.
- His msdp patch: pinned to his donor's libdevicestatus_service; msdp runs fine here.
- His Android-container services (hmos_fusion_manager, rgm_*) from his thermal package.
