# HarmonyOS NEXT on the Nothing Phone (3a)

HarmonyOS 7 (donor: Huawei Pura 70 Pro+, 7.0.0.107) on the Nothing Phone (3a) "asteroids", on our NullDebris 6.6
kernel, slot b. It boots by itself to the desktop, with Wi-Fi, Bluetooth, audio, calls, SMS, mobile data, sensors,
auto-rotate, vibration, flashlight, the lock-screen PIN and third-party apps.

How to flash and run it: [flashing.md](flashing.md). How to work on it: [tooling.md](tooling.md).

## The repository

Split by what each part depends on, so a new phone or donor build touches as little as possible:

| Directory | Layer | Contents |
|---|---|---|
| `common/` | HarmonyOS on any non-Huawei hardware | M1 (`m1/`: init, lpmap, super tools), the boot scripts and init services (`boot/`), framework fixes and no-TEE replacements (`framework/`, `useriam/`, `pinauth/`), `battery/`, `power/`, `services/`, `signing/`, `apps/`, `keyboard/`, `stress/`, `kernel/` (the OpenHarmony IPC changes), `init/`, `include/` |
| `donor/pura70proplus-7.0.0.107/` | this Huawei build | byte patches at fixed offsets (`patches/`), the service lists, the boot pictures |
| `platform/qcom/` | Snapdragon, and Android's vendor blobs run beside HarmonyOS | `gpu/` (Turnip and Zink: Mesa diffs, builds), `display/` (DRM composer, allocator), the Android bridge (`android/`, `audio/`, `bluetooth/`, `sensors/`, `motion/`), `modem/`, `gnss/`, `wifi/`, `media/`, `boot/` (its prep steps and services) |
| `device/asteroids/` | this phone | `device.conf` (its facts, read by the scripts), panel and refresh configs (`display/`), SocPerf values (`power/`), haptics (`vibrator/`), flashlight (`camera/`), the WLAN ini, the speaker backend, CPU tuning and RichTap services (`boot/`), the Android image checksums |
| `build/` | | the image builds (`build-asteroids.sh`, `assemble-boot.sh`, `build-kernel.sh`, `build-ext-modules.sh`), `build-etc.sh`, `extract-donor.sh`, `make-services.py`, `env.sh` |
| `scripts/` | | laptop tools: `port.py` (`./port`), `m0net.py`, `screens.sh`, diagnostics |
| `history/` | | M0, early M1 logs and experiments, kept for the record |

On the phone these merge again: the asteroids partition's `boot/` is the three layers' `boot/` laid over each other.

## Status

| Area | State | File |
|---|---|---|
| Boot chain, M1 init | works; cold boot to desktop alone | [boot.md](boot.md) |
| Partitions, super, data | everything that boots in super; userdata is /data only and is formatted when blank (2026-11-15) | [storage.md](storage.md) |
| Kernel, modules | NullDebris 6.6 with our changes | [kernel.md](kernel.md) |
| Display | works, 120 Hz, direct scanout | [display.md](display.md) |
| GPU (Vulkan, GLES) | works | [gpu.md](gpu.md) |
| Frame pacing, CPU, scheduling | WALT, cgroups, SocPerf; some jank left | [performance.md](performance.md) |
| Framework services | 221 configs; Huawei TEE parts replaced in software | [framework.md](framework.md) |
| Apps | system, preload and self-signed apps run; Huawei attestation is a wall | [apps.md](apps.md) |
| Audio | speakers, mic, call audio | [audio.md](audio.md) |
| Bluetooth | switches on; profiles untested | [bluetooth.md](bluetooth.md) |
| Wi-Fi | works | [wifi.md](wifi.md) |
| Modem, telephony | calls, incoming SMS, data; no VoLTE | [modem.md](modem.md) |
| Location | engine runs, no fix yet | [location.md](location.md) |
| Sensors, auto-rotate | works | [sensors.md](sensors.md) |
| Vibration | works (RichTap) | [vibration.md](vibration.md) |
| Camera, flashlight | photos, 1080p video, zoom, focus, flashlight (Huawei's Camera app) | [camera.md](camera.md) |
| Battery, suspend, freezer, thermal | works; no thermal policy | [power.md](power.md) |
| Lock screen | PIN works; no fingerprint | [lockscreen.md](lockscreen.md) |
| Video decoding and encoding | H.264 and HEVC in hardware, both ways | [media.md](media.md) |
| Hardware inventory | what exists and what's missing | [hardware.md](hardware.md) |
| History, reference ports | | [history.md](history.md) |

## Open items

Missing hardware, largest first:

1. Camera extras: the app's other modes (Pro, Night, Portrait), the flash for photos, stabilisation
   ([camera.md](camera.md)).
2. Fingerprint: the Goodix trusted app in QSEE via Android's fingerprint HAL ([lockscreen.md](lockscreen.md)).
3. VoLTE/IMS: qcrild and Qualcomm's IMS daemons in /android ([modem.md](modem.md)).
4. NFC (ST21NFC + ST54 secure element).
5. Thermal policy: nothing above the kernel's trips ([power.md](power.md)).
6. USB: MTP, host/OTG, USB-C audio.
7. Essential key (code 250 in gpio-keys) mapping; FM radio (RTC6226); Glyph lights (left alone).

Unfinished or unchecked:

- Modem: sending SMS, MMS through telephony, a SIM in slot 2, SIM PIN, emergency calls, cell info, the 5G label.
- GNSS fix outdoors; AGNSS.
- Bluetooth pairing and audio profiles.
- Microphone from apps; echo cancellation in calls.
- Battery drain over hours (unplugged suspend works); a real charger; charging when hot.
- Auto-brightness, proximity in calls, compass, through the UI.
- Video: VP9, zero copy, screen recording (RGBA into the encoder).
- Frame pacing: render_service's per-frame work (blurs, row caches), Turnip's pipeline cache, KGSL allocation cost.
- Boot: the time from the handoff to samgr varies between boots (samgr ready at 15 s on one, 32-35 s on others);
  boot.completed at 28-45 s ([boot.md](boot.md)).
- SceneBoard's death after the first screen-off (not reproduced since 2026-11-12, not closed) ([display.md](display.md)).
- Huawei's Android container set (isulad etc.) runs for nothing: drop it from desktop-services.txt.

Walls (need Huawei's device credentials or TEE): Weather, Petal Maps services, Celia language packs, DroiTong (Android
apps), AppGallery store apps, key attestation.
