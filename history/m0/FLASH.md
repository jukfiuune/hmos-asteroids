# M0: first boot test on slot `_a`

The goal is to boot our 6.6 kernel with a busybox init and get a shell over USB. Android on slot `_b`, `super` and `userdata` are not touched.

The images are in `images/`. Check them with `sha256sum -c images/SHA256SUMS`. To rebuild them, run `~/harmony-build/assemble-m0.sh` on the build PC.

| Image | Contents |
|---|---|
| boot.img | 6.6.142 kernel only (header v4) |
| init_boot.img | busybox 1.37 (Alpine, static) plus `m0/init` |
| vendor_boot.img | merged volcano.dtb, 106 modules (the device's first-stage list plus the USB controller, PHYs and repeaters), device cmdline and bootconfig |
| dtbo.img | asteroids base and pro board overlays |
| vbmeta.img | no descriptors, flags 2 (verification disabled) |

## Before flashing

- Use a USB cable. Wireless adb won't survive the reboot.
- In fastboot, run `fastboot getvar current-slot`. It should print `b`.
- Run `fastboot getvar snapshot-update-status`. It must print `none`. If a Virtual A/B update merge is still pending, let Android finish it first.

## Flash (slot `_a` only)

```bash
adb reboot bootloader
cd ~/Documents/harmony/m0/images
fastboot flash boot_a boot.img
fastboot flash init_boot_a init_boot.img
fastboot flash vendor_boot_a vendor_boot.img
fastboot flash dtbo_a dtbo.img
fastboot flash vbmeta_a vbmeta.img
fastboot --set-active=a
fastboot reboot
```

## What to expect

- If it works, the laptop sees a new USB device "HarmonyPort asteroids M0 shell" (`1d6b:0104`) and `/dev/ttyACM0` appears within about a minute. Connect with `busybox microcom -s 115200 /dev/ttyACM0` or `picocom /dev/ttyACM0`, then run `dmesg`.
- The screen may stay on the boot logo or go black. There's no userspace display yet.
- If nothing appears, M0 failed somewhere before the gadget came up. Go back to Android (below). The init logs to pstore (`/dev/pmsg0`), and the kernel console goes to ramoops, so the next successful boot of our image can read `/sys/fs/pstore`.

## Going back to Android

Hold Vol− and Power until the bootloader screen appears, then:

```bash
fastboot --set-active=b
fastboot reboot
```
