#!/bin/bash
# Assembles the M0 boot chain for slot _a on the build PC: boot (kernel only), init_boot (busybox + m0/init),
# vendor_boot (merged volcano DTB, first-stage + USB modules, device cmdline/bootconfig), vbmeta (verification
# disabled) and dtbo. Expects ~/harmony-build/m0/init and the kernel, stage and dtmerge outputs. Writes out/images/.
set -e
cd ~/harmony-build/src
OUT=$(realpath out); IMG=$OUT/images; W=$OUT/m0-work
R=$(cat out/kernel/include/config/kernel.release)
ST=$OUT/stage/lib/modules/$R
rm -rf $W; mkdir -p $W/init_boot/bin $W/vendor/lib/modules/$R $IMG

# USB controller, PHYs, repeaters and Type-C/pmic_glink from vendor_dlkm, needed for the gadget.
printf '%s\n' qti_pmic_glink.ko ucsi_qti_glink.ko repeater.ko repeater-qti-pmic-eusb2.ko phy-generic.ko \
    phy-msm-snps-eusb2.ko phy-msm-ssusb-qmp.ko dwc3-msm.ko > $W/m0-usb.list

# vendor_boot ramdisk: the first-stage and recovery lists (what gives USB in recovery) plus the USB set and
# everything they depend on, re-indexed with depmod.
want=$(cat device/modules.load.vendor_boot device/modules.load.recovery $W/m0-usb.list | sed 's/\.ko$//')
python3 - "$ST/modules.dep" $want > $W/closure.txt <<'EOF'
import sys
dep = {}
for line in open(sys.argv[1]):
    mod, _, deps = line.partition(":")
    dep[mod.rsplit("/", 1)[-1][:-3].replace("-", "_")] = (mod, deps.split())
paths, todo = set(), [w.replace("-", "_") for w in sys.argv[2:]]
while todo:
    name = todo.pop()
    if name not in dep:
        print("missing " + name, file=sys.stderr)
        continue
    mod, deps = dep[name]
    if mod in paths:
        continue
    paths.add(mod)
    todo += [d.rsplit("/", 1)[-1][:-3].replace("-", "_") for d in deps]
print("\n".join(sorted(paths)))
EOF
(cd $ST && cpio -pdm --quiet $W/vendor/lib/modules/$R < $W/closure.txt)
cp $ST/modules.builtin* $ST/modules.order $W/vendor/lib/modules/$R/
depmod -b $W/vendor $R
cp device/modules.load.vendor_boot $W/vendor/lib/modules/modules.load.vendor_boot
cp $W/m0-usb.list $W/vendor/lib/modules/modules.load.m0-usb
cp device/modules.load.recovery $W/vendor/lib/modules/modules.load.recovery
(cd $W/vendor && find . | cpio -o -H newc --quiet --owner 0:0) | lz4 -l -12 -q > $W/vendor_ramdisk.lz4

# init_boot ramdisk: busybox and the M0 init.
cp ~/harmony-build/tools/busybox/x/bin/busybox.static $W/init_boot/bin/busybox
install -m 0755 ~/harmony-build/m0/init $W/init_boot/init
(cd $W/init_boot && mkdir -p proc sys dev tmp run && find . | cpio -o -H newc --quiet --owner 0:0) \
    | lz4 -l -12 -q > $W/init_boot_ramdisk.lz4

# Kernel command line and bootconfig as in the stock vendor_boot (Asteroids_C5.0-261023-2146), plus the BoardConfig
# extras. "bootconfig" makes the kernel read the bootconfig section.
CMDLINE="video=vfb:640x400,bpp=32,memsize=3072000 nosoftlockup log_buf_len=1M ignore_loglevel printk.devkmsg=on \
firmware_class.path=/vendor/firmware,/vendor/firmware_mnt/image qcom_geni_serial.con_enabled=0 bootconfig"
printf '%s\n' androidboot.hardware=qcom androidboot.memcg=1 androidboot.usbcontroller=a600000.dwc3 \
    androidboot.load_modules_parallel=true androidboot.vendor.qspa=true androidboot.console=0 > $W/bootconfig

mkbootimg --header_version 4 --kernel out/kernel/arch/arm64/boot/Image.gz --output $IMG/boot.img
# ABL hands init_boot's OS version and patch level to KeyMint; stock carries 17.0.0 / 2026-11, and a blank or older
# value can make ABL abort the boot and mark the slot unbootable.
mkbootimg --header_version 4 --os_version 17.0.0 --os_patch_level 2026-11 --ramdisk $W/init_boot_ramdisk.lz4 \
    --output $IMG/init_boot.img
mkbootimg --header_version 4 --pagesize 4096 --base 0x00000000 --kernel_offset 0x00008000 \
    --ramdisk_offset 0x01000000 --tags_offset 0x00000100 --dtb_offset 0x01f00000 \
    --dtb out/dtmerge/out/volcano.dtb --vendor_cmdline "$CMDLINE" --vendor_bootconfig $W/bootconfig \
    --vendor_ramdisk $W/vendor_ramdisk.lz4 --vendor_boot $IMG/vendor_boot.img
# vbmeta: the stock one, flashed with --disable-verity --disable-verification (see m0/FLASH.md).
rm -f $IMG/vbmeta.img

# Partition sizes from the device BoardConfig: boot and vendor_boot 96 MiB, init_boot 8 MiB, dtbo 24 MiB.
check() { [ $(stat -c %s $1) -le $2 ] || { echo "$1 is larger than its partition"; exit 1; }; }
check $IMG/boot.img $((0x6000000)); check $IMG/vendor_boot.img $((0x6000000))
check $IMG/init_boot.img $((0x800000)); check $IMG/dtbo.img $((0x1800000))
echo "modules in vendor_boot: $(wc -l < $W/closure.txt)"
(cd $IMG && ls -la && sha256sum *.img > SHA256SUMS)
