# HarmonyOS NEXT on the Nothing Phone (3a)

A port of HarmonyOS 7 (Huawei's HarmonyOS NEXT line; donor build: Pura 70 Pro+, 7.0.0.107) to the Nothing Phone (3a)
"asteroids" (A059, Snapdragon 7s Gen 3), on a LineageOS 6.6 kernel. It boots by itself to the HarmonyOS desktop with
Wi-Fi, Bluetooth, audio, calls, SMS, mobile data, sensors, vibration, the camera and the lock-screen PIN, and runs
Huawei's Android container with Google Play. What works and what doesn't: [docs/README.md](docs/README.md).

This repository holds scripts and patches only. Huawei's firmware, Nothing's firmware and Google's apps are not in
it: you download them yourself, and the scripts check each file against the SHA-256 below and extract what they
need. Two other repositories go with this one:

- **hmos-asteroids-kernel**: the kernel changes (OpenHarmony's binder and access-token driver, the one-way binder
  sender pid, the haptic and Wi-Fi module fixes) and the kernel build. `build/fetch-sources.sh` here applies the same
  changes; that repository has them on their own.
- **hmos-asteroids-apps**: the on-screen keyboard and the Petrelgram/Harlgram (Telegram) builds.

> **Warning.** This replaces Android on slot b of your phone and needs an unlocked bootloader. It only writes slot b's
> boot partitions (boot, vendor_boot, init_boot, dtbo) and super. It never writes the firmware partitions (xbl, abl,
> tz, modem). Slot a keeps Android, and `scripts/restore-b.sh` puts Android back on slot b. Back up your data first.
> You use this at your own risk.

## What you need

- A Nothing Phone (3a) (A059) with the bootloader unlocked, on Nothing OS `Asteroids_C5.0-261023-2146`.
- A **build machine**: x86-64 Linux, about 250 GB free, 16 GB RAM or more. It needs git, curl, unzip, python3,
  clang/lld/llvm, make, bc, flex, bison, cpio, lz4, meson, ninja, pkg-config, Java 17 (for hap-sign-tool),
  e2fsprogs (debugfs), and `sudo` that works without a password prompt (`sudo -n`) for loop mounts and for
  copying root-owned donor files.
- A **laptop** on USB to the phone, with python3, `fastboot` and ssh access to the build machine. One machine can be
  both: set `BUILD_HOST=local`.

## 1. Set up

On the laptop:

```bash
git clone <this repository> harmony && cd harmony
cp port.conf.example port.conf      # BUILD_HOST=user@buildhost (or local), BUILD_ROOT=harmony-build
./port sync                          # copies the repo to BUILD_ROOT/repo on the build machine
```

All builds read the repo from `BUILD_ROOT/repo` (`$HARMONY_BUILD` in the build scripts, default `~/harmony-build`).
Run `./port sync` after every change. The commands in steps 2 to 4 run on the build machine, in
`$HARMONY_BUILD/repo`.

## 2. Get the firmware

| What | File | SHA-256 | Where it goes |
|---|---|---|---|
| HarmonyOS donor: Huawei Pura 70 Pro+ (HBN-AL10) 7.0.0.107, full base OTA (7 050 120 690 bytes) | `update_full_base.zip` from `http://update.dbankcdn.com/download/data/pub_13/HWHOTA_hota_900_9/6e/v3/v8JuxUb2TBejup8P2vgrrQ/full/update_full_base.zip` | `36fbb2d601ddf94a4b6542f5a58ecf5337344c0ad3cdd2090e4e894e771a154b` | argument to `build/unpack-donor.sh` |
| The same release's preload app package | `update_full_preload_HBN-AL10_all_cn_R3.zip` | `cc7156075c89a4cbef411a78be0ee002ca545bda5e2d9e80e585357c518affe0` | argument to `build/unpack-donor.sh` |
| Nothing OS `Asteroids_C5.0-261023-2146`, the logical partitions (from [Nothing Archive](https://github.com/spike0en/nothing_archive)) | `system.img`, `system_ext.img`, `vendor.img`, `odm.img` | in `device/asteroids/android/inputs.sha256` | `$HARMONY_BUILD/android-b/` as `system_b.img`, `system_ext_b.img`, `vendor_b.img`, `odm_b.img` |
| Android container image: Huawei BRE-AL80 full base OTA (optional, for Android apps) | `update_full_base.zip` (URL in `common/apps/build-anco-preset.sh`) | `d1703a5985eccaac8ec1e3a067debb50f0fd23a02ee9d94250c3c2f29ec8e795` | `build-anco-preset.sh` downloads it |
| Google apps for the container (optional) | `MindTheGapps-16.0.0-arm64-20261023_034858.zip` | `6929593937cd85541228e4e5db9cbb5455338bf33c75eabf610de632ac155810` | `$HARMONY_BUILD/fw/gapps/dl/` |

Then:

```bash
build/fetch-sources.sh                        # kernel, OpenHarmony SDK and sources, Mesa, tools, at pinned commits
build/unpack-donor.sh ~/update_full_base.zip ~/update_full_preload_HBN-AL10_all_cn_R3.zip
(cd $HARMONY_BUILD/android-b && sha256sum -c $HARMONY_BUILD/repo/device/asteroids/android/inputs.sha256)
device/asteroids/vibrator/extract-firmware.sh # the haptics firmware, from Android's vendor image
```

`unpack-donor.sh` mounts the donor's images read-only under `donor/.../mnt/`. After the build machine reboots, run
`build/unpack-donor.sh --mount` again.

## 3. Make your signing keys

Apps outside Huawei's store are signed with your own root certificate, which the build adds to the phone's trust
store:

```bash
mkdir -p $HARMONY_BUILD/tools/asteroids-signing && cd $HARMONY_BUILD/tools/asteroids-signing
bash $HARMONY_BUILD/repo/common/signing/gen-pki.sh
```

The keys stay in that directory. Never publish them. Once the phone runs (step 5), put its UDID in `udid` there:
`./port sh 'chroot /hmos-root /system/bin/bm get --udid'`.

## 4. Build

```bash
build/build-kernel.sh && build/build-ext-modules.sh   # kernel and vendor modules
```

Then, from the laptop:

```bash
./port build boot asteroids android    # boot images (assemble-boot.sh), the asteroids partition, Android's helpers
```

Each piece's details are in [docs/](docs/README.md): [kernel.md](docs/kernel.md), [boot.md](docs/boot.md),
[gpu.md](docs/gpu.md), [framework.md](docs/framework.md), [apps.md](docs/apps.md).

## 5. Install

1. Put the phone in the bootloader (Vol- and Power) and run `scripts/flash-boot.sh`. It writes boot_b, vendor_boot_b,
   init_boot_b and dtbo_b, sets slot b active and reboots into M1, the port's first-stage init.
2. With M1 up (`./port sh 'uname -a'` answers over the USB network), write super's slot b partitions:
   `./port deploy system vendor sys_prod chip_prod preload cust asteroids android`. On a later update, `./port deploy`
   only the parts that changed.
3. The phone reboots into HarmonyOS's setup wizard. `fastboot erase userdata` resets it to a factory state.
4. Optional, for Android apps: `common/apps/build-anco-preset.sh` and `common/apps/build-anco-gapps.sh` on the build
   machine, then `common/apps/stage-anco.sh` and `common/apps/stage-anco-gapps.sh` from the laptop
   ([docs/apps.md](docs/apps.md)).

To go back to Android on slot b, follow [docs/flashing.md](docs/flashing.md), section 4, or run
`scripts/restore-b.sh`.

## Known gaps

The port is rebuilt from these steps, but not every step has run on a clean machine:

- `build/build-asteroids.sh` expects some files in `$HARMONY_BUILD/m2` that no script makes yet: the Turnip build
  directory `build-tu-ohos` (Meson setup over `m2/mesa`, see [docs/gpu.md](docs/gpu.md)), the sysroots
  (`platform/qcom/gpu/mk-sysroots.sh`), `bootpic.zip` (`donor/.../bootpic/make-bootpic.py`) and the `hmos-stage` seed.
  The dtbo image (`src/out/images/dtbo.img`) comes from the devicetree build.
- Step 5.2 (writing all of super from M1 on a phone that still has Android in super) is the documented path but
  hasn't run from scratch.
- DroiTong (Huawei's Android app front end) has to be supplied as a decrypted HAP (`common/apps/sign-droitong.sh`).

## Layout

| Directory | Contents |
|---|---|
| `common/` | HarmonyOS on non-Huawei hardware: M1 init and super tools, boot scripts and services, framework fixes, signing |
| `platform/qcom/` | Snapdragon: GPU (Turnip, Zink), display, the Android bridge (audio, Bluetooth, sensors), modem, GNSS, Wi-Fi, media |
| `device/asteroids/` | this phone: device facts, display, power, vibration, camera, Wi-Fi, checksums of the Android inputs |
| `donor/pura70proplus-7.0.0.107/` | the donor build: byte patches, service lists, image checksums |
| `build/` | image builds, source fetching, donor unpacking |
| `scripts/` | laptop tools: `./port`, the USB shell, flashing, diagnostics |
| `docs/` | one file per subsystem |
| `history/` | early milestones' logs and experiments |

## Licence

Apache-2.0 ([LICENSE](LICENSE)). The kernel changes are GPL-2.0. Third-party parts and their licences:
[THIRD-PARTY.md](THIRD-PARTY.md).
