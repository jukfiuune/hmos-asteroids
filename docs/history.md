# History and references

## How it went

| Date | Milestone |
|---|---|
| 2026-11-11 | M0: our 6.6 kernel and busybox init to a USB shell. M1: HarmonyOS 7 init as PID 1, samgr and param_watcher over binder with the OHOS access-token changes. M2 first stage: Turnip on KGSL, frames on the panel. Boot animation through HarmonyOS's own display path. |
| 2026-11-12 | Desktop: SceneBoard, lock screen, setup wizard, home screen, Settings. Battery, Wi-Fi, Bluetooth, audio, video decoding, sensors, GLES, auto-rotate, photos. |
| 2026-11-13 | Third-party apps launch (kernel one-way pid), HUKS, preload and sideloaded apps, keyboard, Telegram, modem with SIM: calls, SMS, call audio; suspend, freezer, CPU governor. |
| 2026-11-14 | Mobile data, GNSS library, cgroups, SocPerf, vibration (RichTap), chip name, flashlight, lock-screen PIN, donor images in super. |

Logs of the first handoffs: `history/m1-handoff1-console.txt`, `m1/handoff2` to `m1/handoff5`.

## The original plan, and what changed

The first plan (2026-11-11) was: HarmonyOS on slot `_a` beside Android on `_b`, images as loop files on userdata,
super untouched, a Mate 60/80 Pro donor, one Mesa build with Turnip, Zink and llvmpipe, `TU_DEBUG=sysmem,nocb`.
What happened instead: slot `_a` crashes ABL, so everything is on `_b` and Android is gone; HarmonyOS fills userdata and,
since 2026-11-14, super; the donor is the Pura 70 Pro+ 7.0.0.107; Turnip is Mesa main without debug flags, and Zink is
built separately from OpenHarmony's 25.0.1 fork; no llvmpipe. Milestones M3-M5 (touch, GPU tuning, Wi-Fi/audio/battery)
were all reached inside M2.

From the iOS port's GPU work (`~/Documents/iosasteroids`, `iosonandng/docs/asteroids-gpu-gen8.md`): unmodified Turnip
ran on this A810 through a different KMD; `turnip-a810-sysmem-ccu.patch` came from there; linear textures with a
foreign 64-byte row pitch scrambled small textures, so the allocator hands Turnip its own layouts.

## Reference ports

- **sheng** (AnyHarmonyOSNext/anyhmos_xiaomi_sheng, a Xiaomi tablet; checkout `ref/anyhmos_xiaomi_sheng` at 8095782,
  2026-11-06): Huawei system/vendor/sys_prod unmodified, GKI with IPC exports, shims for display, touch, audio (PAL),
  Wi-Fi, BT, battery, sensors, UserIAM, Turnip + Zink. Most of our Android-bridge designs are his. His feature table
  (README.en.md) has no modem, HDR or camera either. What we use, what we don't: [framework.md](framework.md).
- **Redmi K90 Pro Max** HarmonyOS 7 port (notes the user shared; CLAUDE.zip, HEAT_CGROUP.md): same RIL design with an
  AT proxy ("hmatd") that got SMS and VoLTE; found the cgroup gap; stopped at "executor is unregistered" for the PIN.
- **UBports ofono-binder-plugin-ext-qti**: a reference for qcrild's IMS interface (VoLTE), PC `~/harmony-build/ref`.

## Facts carried over

- Touch: FocalTech FT3683G on SPI SE0 (`fts_ts`), IRQ GPIO 19, reset GPIO 20; firmware in the controller's flash;
  1:1 with 1080x2392. The vendor `focaltech` driver.
- Keys: Power and Vol- on the PMK8550 PON, Vol+ on PMXR2230 GPIO 6, the Essential (AI) key on TLMM GPIO 71.
- Android (before it was removed) had KernelSU v3.3.1 in LKM mode (`init_boot_ksu.img`).
