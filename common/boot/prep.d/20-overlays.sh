# /vendor: our files (composer and allocator VDIs, Turnip as Huawei's Vulkan driver, firmware, ueventd rules,
# parameters) over the donor's, plus the serial number as ohos.boot.sn (the device UDID is derived from it, and
# useriam won't start its driver without one; it comes from the bootloader, so it is written here).
mkdir -p $G/vendor/etc/param
SN=$(sed -n 's/^androidboot.serialno = "\(.*\)"$/\1/p' /proc/bootconfig)
[ -n "$SN" ] && echo "ohos.boot.sn=$SN" > $G/vendor/etc/param/asteroids-sn.para
mount -t overlay -o lowerdir=$G/vendor:$A/vendor:/hmos/vendor overlay $R/vendor
# The overlay uncovers vendor/etc/init, which M1 hid: hide it again, or every Huawei vendor service starts.
mount -t tmpfs -o mode=0755,size=4m hmos-init $R/vendor/etc/init
# /system: ours copied into the root's RAM upper (the bootanimation launcher, sheng's shim, HDI publishers, the
# framework patches).
cp -a $A/system/. $R/system/
# The asteroids partition itself, for the init services that run our scripts (svc/), and the Android helpers' root.
mkdir -p $R/asteroids $R/android
mount --bind $A $R/asteroids
