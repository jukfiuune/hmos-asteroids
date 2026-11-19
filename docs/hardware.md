# Hardware

| | |
|---|---|
| Model | Nothing Phone (3a), A059, EEA (`androidboot.pbid` Base), codename asteroids |
| SoC | SM7635 "volcano", Snapdragon 7s Gen 3: 4+3+1 cores (policy0/4/7) |
| GPU | Adreno 810 (gen8, chip 0x44010000) |
| RAM / storage | 12 GB / UFS, 225 GB userdata |
| Stock software | Android 17 (CQ2A), GKI 6.1, vendor API 34; bootloader unlocked |
| Our kernel | NullDebris 6.6 ([kernel.md](kernel.md)) |

Measured on the phone with stock Android on 2026-11-11; the inventory below was rechecked on the running port on
2026-11-14 (I2C/SPI devices, modules, device nodes, device tree).

## Inventory and status

| Part | Device | Status | Notes |
|---|---|---|---|
| Display | Visionox RM69220, DSI, 1080x2392, 120 Hz | works | [display.md](display.md); HDR unused |
| GPU | Adreno 810, KGSL | works | Vulkan (Turnip), GLES (Zink) [gpu.md](gpu.md) |
| Touch | FocalTech FT3683G, SPI (`fts_ts`) | works | |
| Keys | power, vol-/vol+ (PMIC), Essential key (gpio-keys, code 250) | power and volume work | Essential key: unmapped in HarmonyOS |
| Wi-Fi | QCA6750 (WPSS, qcacld) | works | [wifi.md](wifi.md) |
| Bluetooth | WCN6750 | switches on | profiles untested [bluetooth.md](bluetooth.md) |
| Modem | MPSS, dual SIM | calls, SMS in, data | VoLTE, 5G label, SMS out missing [modem.md](modem.md) |
| GNSS | modem's QMI LOC | engine runs | no fix yet [location.md](location.md) |
| Speakers | 2 × tfa98xx (I2C 13-0034/35) | works | [audio.md](audio.md) |
| Earpiece, mic | WCD9378 | works in calls | app recording untested |
| USB-C audio | FSA4480 switch (I2C 7-0042) | not tried | |
| Vibration | RT6010 LRA (I2C 8-005f) | works | RichTap [vibration.md](vibration.md) |
| Sensors | LSM6DSV, MMC56x3x, STK3A5x (on the ADSP hub) | works | [sensors.md](sensors.md) |
| Battery, charging | PM charger via ADSP (qti_battery_charger) | level and charging shown | thermal policy missing [power.md](power.md) |
| Suspend | s2idle | works unplugged | drain not measured |
| Video decode and encode | msm_video (V4L2 M2M) | H.264, HEVC both ways | VP9 [media.md](media.md) |
| Flash LED | PMIC flash (torch_0/3) | works as torch | [camera.md](camera.md) |
| Cameras | Spectra, 4 sensors (CCI0 ×2, CCI1 ×2) | photos and video (CamX bridge) | other modes [camera.md](camera.md) |
| Fingerprint | Goodix under-display (`goodix_fp`) | **missing** | matching runs in a QSEE trusted app [lockscreen.md](lockscreen.md) |
| NFC | ST21NFC (I2C 1-0008) + ST54 secure element (SPI) | **missing** | needs Android's NFC HAL bridge or HarmonyOS's nfc HDI on the kernel driver |
| Glyph lights | aw20036 LED driver (I2C 7-003a) | not touched | left alone at the user's request |
| FM radio | RTC6226 (I2C 7-0064) | **missing** | HarmonyOS has no FM radio app |
| USB | dwc3, peripheral mode | debug gadget only | MTP, host/OTG, charging protocols not done |
| Thermal | 59 zones, 73 cooling devices | kernel only | no userspace policy |
| CDSP | remoteproc `32300000.remoteproc-cdsp` | offline at boot | the camera HAL needs it (motion estimation); the CamX test root starts it |

## Facts

- ramoops 4 MiB at 0x81f20000; ABL log at 0x81ce4000; Gunyah virtual watchdog (11 s bark, never unload).
- Touch: IRQ GPIO 19, reset GPIO 20; firmware in the controller's flash.
- Keys: Power and Vol- on the PMK8550 PON, Vol+ on PMXR2230 GPIO 6, the Essential key on TLMM GPIO 71.
- Panel camera hole: a 43 px square at (519, 47) from the device tree's cutout path.
- The bootloader picks NullDebris's asteroids-base or asteroids-pro overlay (same board ID, OEM ID 1 or 2).

Hardware notes from the iOS port: `~/Documents/iosasteroids/iosonandng/docs/asteroids-*.md`.
