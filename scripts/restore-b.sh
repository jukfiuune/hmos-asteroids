#!/bin/bash
# Restores Android on slot b from fastboot (bootloader): stock boot, vendor_boot and dtbo, plus the KernelSU init_boot.
# Run on the build PC with the phone in fastboot. Pass -w to also format userdata, which Android needs once after
# the userdata split. Images: $HARMONY_BUILD/restore-android/.
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
cd $HB/restore-android
sha256sum -c SHA256SUMS
fastboot flash boot_b boot.img
fastboot flash vendor_boot_b vendor_boot.img
fastboot flash init_boot_b init_boot_ksu.img
fastboot flash dtbo_b dtbo.img
fastboot --set-active=b
[ "$1" = -w ] && fastboot -w
fastboot reboot
