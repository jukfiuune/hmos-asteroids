#!/bin/bash
# Flashes the M1 boot chain to slot b from the bootloader (phone in fastboot: Vol- and Power) and makes b active: the
# first step of an install, and how a new boot chain goes on when M1 isn't running (with M1 running, ./port deploy
# boot init_boot vendor_boot writes them in place). Takes build/assemble-boot.sh's images from the build machine
# (port.conf). Only boot_b, vendor_boot_b, init_boot_b and dtbo_b are written; slot a and the firmware are left alone.
#
#   flash-boot.sh [--no-reboot]
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
. $R/scripts/conf.sh
O=$R/out/boot-chain
rm -rf $O && mkdir -p $O
pc_get $O src/out/images-m1/{boot,vendor_boot,init_boot,dtbo}.img
(cd $O && sha256sum *.img)
fastboot devices | grep -q . || { echo "no phone in fastboot" >&2; exit 1; }
for p in boot vendor_boot init_boot dtbo; do fastboot flash ${p}_b $O/$p.img; done
fastboot --set-active=b
[ "$1" = --no-reboot ] || fastboot reboot
