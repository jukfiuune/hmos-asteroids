#!/bin/bash
# Assembles the M1 boot chain on the build PC: boot (harmony kernel), init_boot (busybox, common/m1/init and the allowlisted
# HarmonyOS service configs), vendor_boot (merged volcano DTB, every module on the device's vendor_boot, recovery and
# vendor_dlkm lists plus their dependencies, device cmdline/bootconfig) and dtbo. Stages the modules from the current
# kernel and external module builds first. Takes M1 from the repo's common/m1/ (init, lpmap.c, migrate-userdata.sh, etc-init: the fallback service set). Writes out/images-m1/.
set -e
. "$(dirname "$0")/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
cd $HB/src
OUT=$(realpath out); IMG=$OUT/images-m1; W=$OUT/m1-work
R=$(cat out/kernel/include/config/kernel.release)
K=$(realpath kernel); O=$OUT/kernel
export ARCH=arm64 LLVM=1 LLVM_IAS=1 TARGET_BOARD_PLATFORM=volcano

# Stage all modules, signed and stripped, from this kernel build (task_struct changed, so nothing older fits).
rm -rf $OUT/stage
make -s -C kernel O=$O INSTALL_MOD_PATH=$OUT/stage INSTALL_MOD_STRIP=1 modules_install
for m in $(sed -n '/^TARGET_KERNEL_EXT_MODULES/,/^$/p' device/BoardConfig.mk | grep -oE '(noth|qcom)/[^ \\]+'); do
    make -s -C $K/../kernel-modules/$m KERNEL_SRC=$K M=../kernel-modules/$m O=$O OUT_DIR=$O \
        INSTALL_MOD_PATH=$OUT/stage INSTALL_MOD_STRIP=1 KERNEL_UAPI_HEADERS_DIR=$O modules_install
done
# Ours, built with the asteroids partition (build/build-asteroids.sh): kstate.ko, the app freezer's netlink channel
# (power/kstate.c), and hwlog.ko, stand-ins for Huawei's log devices (kernel/hwlog/hwlog.c). The patched icnss2 and
# hapticdrv come from the trees above (wifi/icnss2-no-dms-mac.patch,
# vibrator/hapticdrv-chip-init.patch, applied there).
install -D -m 644 $WORK/out-power/kstate.ko $OUT/stage/lib/modules/$R/extra/kstate.ko
install -D -m 644 $WORK/out-kernel/hwlog.ko $OUT/stage/lib/modules/$R/extra/hwlog.ko
depmod -b $OUT/stage $R
ST=$OUT/stage/lib/modules/$R
rm -rf $W; mkdir -p $W/init_boot/bin $W/init_boot/m1 $W/vendor/lib/modules/$R $IMG

# USB controller, PHYs, repeaters and Type-C/pmic_glink, as in M0.
printf '%s\n' qti_pmic_glink.ko ucsi_qti_glink.ko repeater.ko repeater-qti-pmic-eusb2.ko phy-generic.ko \
    phy-msm-snps-eusb2.ko phy-msm-ssusb-qmp.ko dwc3-msm.ko > $W/m1-usb.list

# vendor_boot ramdisk: the three load lists, the USB set and their dependencies, re-indexed with depmod.
want="$(cat device/modules.load.vendor_boot device/modules.load.recovery device/modules.load.vendor_dlkm \
       $W/m1-usb.list | sed 's/\.ko$//') kstate hwlog"
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
for l in vendor_boot recovery vendor_dlkm; do cp device/modules.load.$l $W/vendor/lib/modules/; done
# kstate before the WLAN driver: both create netlink unit 30 (the driver's is an unused diagnostics socket).
sed -i 's/^qca_cld3_qca6750\.ko$/kstate.ko\n&/' $W/vendor/lib/modules/modules.load.vendor_dlkm
grep -q '^kstate.ko$' $W/vendor/lib/modules/modules.load.vendor_dlkm
echo hwlog.ko >> $W/vendor/lib/modules/modules.load.vendor_dlkm
# Firmware the drivers M1 loads ask for before HarmonyOS's /vendor is in place: the speaker amplifiers' container
# (tfa98xx), the haptic chip's config and effects (hapticdrv keeps asking until it gets them), the WLAN driver's ini.
F=$W/vendor/vendor/firmware
install -D -m 644 $WORK/hmos-stage/m2/vendor/firmware/tfa98xx.cnt $F/tfa98xx.cnt
[ -d $HB/fw/vibrator ] || bash $DEVICE/vibrator/extract-firmware.sh
install -m 644 $HB/fw/vibrator/*.bin -t $F
install -D -m 644 $DEVICE/wifi/WCNSS_qcom_cfg.ini $F/wlan/qca_cld/qca6750/WCNSS_qcom_cfg.ini
cp $W/m1-usb.list $W/vendor/lib/modules/modules.load.m1-usb
(cd $W/vendor && find . | cpio -o -H newc --quiet --owner 0:0) | lz4 -l -12 -q > $W/vendor_ramdisk.lz4

# init_boot ramdisk: busybox, lpmap (maps the partitions in super, common/m1/lpmap.c), the M1 init, the userdata migration
# (common/m1/migrate-userdata.sh) and the M1 service set (the asteroids partition's m1/etc-init replaces it).
cp $HB/tools/busybox/x/bin/busybox.static $W/init_boot/bin/busybox
N=$HB/ohos/sdk/native
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -static -s \
    $COMMON/m1/lpmap.c -o $W/init_boot/bin/lpmap
install -m 0755 $COMMON/m1/init $W/init_boot/init
install -D -m 0755 $COMMON/m1/migrate-userdata.sh $W/init_boot/m1/migrate-userdata.sh
cp -r $COMMON/m1/etc-init $W/init_boot/m1/etc-init
(cd $W/init_boot && mkdir -p proc sys dev tmp run && find . | cpio -o -H newc --quiet --owner 0:0) \
    | lz4 -l -12 -q > $W/init_boot_ramdisk.lz4

# Kernel command line and bootconfig as in the stock vendor_boot, plus the BoardConfig extras (see assemble-m0.sh).
CMDLINE="video=vfb:640x400,bpp=32,memsize=3072000 nosoftlockup log_buf_len=1M ignore_loglevel printk.devkmsg=on \
firmware_class.path=/vendor/firmware,/vendor/firmware_mnt/image qcom_geni_serial.con_enabled=0 bootconfig"
printf '%s\n' androidboot.hardware=qcom androidboot.memcg=1 androidboot.usbcontroller=a600000.dwc3 \
    androidboot.load_modules_parallel=true androidboot.vendor.qspa=true androidboot.console=0 > $W/bootconfig

# arm64.nopauth: Cangjie runtimes bundled in apps (Petrelgram, Weibo) sign return addresses with PACIASP; Kirin has no
# PAC so they stay plain there, but this Snapdragon signs them and the runtime's stack walk then crashes (SIGSEGV in
# StackInfo::AnalyseAndSetFrameType). With PAC off the instructions are no-ops, as on Kirin.
# loop.max_part=0 overrides the bootloader's loop.max_part=7 (ours comes last): the anco container's apexd creates its
# loop nodes as minor = index, which only matches loop devices without partition minors.
mkbootimg --header_version 4 --kernel out/kernel/arch/arm64/boot/Image.gz --cmdline "arm64.nopauth loop.max_part=0" \
    --output $IMG/boot.img
mkbootimg --header_version 4 --os_version 17.0.0 --os_patch_level 2026-11 --ramdisk $W/init_boot_ramdisk.lz4 \
    --output $IMG/init_boot.img
mkbootimg --header_version 4 --pagesize 4096 --base 0x00000000 --kernel_offset 0x00008000 \
    --ramdisk_offset 0x01000000 --tags_offset 0x00000100 --dtb_offset 0x01f00000 \
    --dtb out/dtmerge/out/volcano.dtb --vendor_cmdline "$CMDLINE" --vendor_bootconfig $W/bootconfig \
    --vendor_ramdisk $W/vendor_ramdisk.lz4 --vendor_boot $IMG/vendor_boot.img
cp out/images/dtbo.img $IMG/dtbo.img

# Partition sizes from the device BoardConfig: boot and vendor_boot 96 MiB, init_boot 8 MiB, dtbo 24 MiB.
check() { [ $(stat -c %s $1) -le $2 ] || { echo "$1 is larger than its partition"; exit 1; }; }
check $IMG/boot.img $((0x6000000)); check $IMG/vendor_boot.img $((0x6000000))
check $IMG/init_boot.img $((0x800000)); check $IMG/dtbo.img $((0x1800000))
echo "modules in vendor_boot: $(wc -l < $W/closure.txt)"
(cd $IMG && ls -la && sha256sum *.img > SHA256SUMS)
