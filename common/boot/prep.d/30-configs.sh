# Configuration bound over the donor's: the files the build made (build-etc.sh, from the donor's own) and ours.
b() { mount --bind $A/etc/$1 $R/$2 || log "$1 not bound over $2"; }
b bootanimation_custom_config.json sys_prod/etc/bootanimation/bootanimation_custom_config.json
b oobe sys_prod/etc/oobe
b distributed_file.para system/etc/param/distributed_file.para
b display_manager_config.xml sys_prod/etc/window/resources/display_manager_config.xml
b hgm_policy_config.xml sys_prod/etc/graphic/hgm_policy_config.xml
b socperf_resource_config.xml sys_prod/etc/soc_perf/socperf_resource_config.xml
b socperf_boost_config.xml sys_prod/etc/soc_perf/socperf_boost_config.xml
b bluetooth_service.json system/profile/bluetooth_service.json
b audio_policy_config.xml vendor/etc/audio/audio_policy_config.xml
b CameraProfiles.json sys_prod/etc/camera/CameraProfiles.json
b baselayer.cfg system/etc/cust/baselayer.cfg
b ril-common.para chip_prod/etc/ril_param/common.para
b security/trusted_root_ca.json system/etc/security/trusted_root_ca.json
b security/trusted_apps_sources.json system/etc/security/trusted_apps_sources.json
# Bluetooth: sheng's vendor library in place of the donor's, loaded by the donor's HCI service; it reaches Qualcomm's
# HAL through svc/bt-bridge.sh.
mount --bind $A/android/bluetooth/libbt_vendor_sheng.so $R/vendor/lib64/libbt_vendor.z.so || log "libbt_vendor not bound"
# The phone's name and model: the bootloader's variant (androidboot.pbid, Base or Pro, what LineageOS keys on), else
# the device tree's model (the base or Pro overlay, chosen by OEM ID).
PBID=$(sed -n 's/^androidboot.pbid = "\(.*\)"$/\1/p' /proc/bootconfig)
if [ "$PBID" = Pro ] || { [ -z "$PBID" ] && grep -q Pro /proc/device-tree/model 2>/dev/null; }; then
    b hw_defaults-$MODEL_PRO.para sys_prod/etc/param/hw_defaults.para
else
    b hw_defaults-$MODEL_BASE.para sys_prod/etc/param/hw_defaults.para
fi
# The chip's name for apps: deviceInfo.chipType is ohos.boot.hardware, which M1 passes as "asteroids" in the command
# line and bootconfig it binds over /proc (any name without Kirin configs will do). Init takes the value up to the
# first space, so the words are joined by no-break spaces, which apps show as spaces.
C=$(echo "$CHIP" | sed "s/ /$(printf '\302\240')/g")
if [ -f /hmos-ram/cmdline ]; then
    c=$(sed "s/ohos.boot.hardware=asteroids/ohos.boot.hardware=$C/" /hmos-ram/cmdline)
    echo "$c" > /hmos-ram/cmdline
    c=$(sed "s/^ohos.boot.hardware = \"asteroids\"$/ohos.boot.hardware = \"$C\"/" /hmos-ram/bootconfig)
    echo "$c" > /hmos-ram/bootconfig
fi
