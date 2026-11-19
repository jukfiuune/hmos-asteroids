#!/bin/bash
# Makes the configuration files the asteroids partition binds over the donor's, from the donor's own (extracted by
# build/extract-donor.sh) and device.conf: what prep.sh used to rewrite on every boot. Output: <out>/etc/...
#   build-etc.sh <out>
set -e
. "$(dirname "$0")/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
O=${1:?usage: build-etc.sh <out>}/etc
H=$(cd "$(dirname "$0")" && pwd)
X=$HB/donor/pura70proplus-7.0.0.107/x
. $DEVICE/device.conf
rm -rf $O && mkdir -p $O/oobe

# Storage without hmdfs (this kernel has none; sheng's apply-storage-local-v1.sh): with distributed files off,
# storage_daemon mounts each user's storage locally, so user 100 starts and apps get unlocked sandboxes.
sed 's/^const.distributed_file_property.enabled=true$/const.distributed_file_property.enabled=false/' \
    $X/system/system/etc/param/distributed_file.para > $O/distributed_file.para
# The setup wizard offline (sheng's oobe_page_offline.json): no Wi-Fi, activation-lock or account pages.
cp $X/sys_prod/etc/oobe/hwupgrade_extend_pages.json $O/oobe/
cp $COMMON/init/oobe_page.json $O/oobe/
# The phone's name and model per variant; NearLink off (no such radio); the refresh-rate choice in Settings; no HDR
# photos (Gallery decodes them into 10-bit buffers our allocator and Turnip don't have). Brand and manufacturer stay
# HUAWEI, which Huawei's services check. prep.sh binds the one for the running variant.
for v in BASE PRO; do
    eval "name=\$NAME_$v model=\$MODEL_$v"
    { sed -e 's/^const.nearlink.enable=true$/const.nearlink.enable=false/; /^const.settings.refresh_rate_seenable=/d' \
          -e 's/^const.display.hdr_picture_support=true$/const.display.hdr_picture_support=false/' \
          -e 's/^const.display.support_aihdr_picture=true$/const.display.support_aihdr_picture=false/' \
          -e 's/^const.display.aihdr_picture_default_enable=true$/const.display.aihdr_picture_default_enable=false/' \
          -e "s/^const.product.name=.*/const.product.name=$name/; /^const.product.model=/d" \
          $X/sys_prod/etc/param/hw_defaults.para
      echo const.settings.refresh_rate_seenable=true
      echo const.product.model=$model; } > $O/hw_defaults-$model.para
done
# Bluetooth's SA created when its process starts (asteroids-desktop.cfg starts it once the HCI service is up), as
# sheng's bluetooth_service_v136.json.
sed 's/"run-on-create" *: *false/"run-on-create": true/; s/"auto-restart" *: *true/"auto-restart": false/' \
    $X/system/system/profile/bluetooth_service.json > $O/bluetooth_service.json
# Audio policy: the speaker on the normal pipe only (sheng's VDI implements the plain render; the donor also offers
# offload, which audio_server picks for music and the VDI refuses, fast and VoIP-direct).
sed -e '/<pipe name="offload_output"/,/<\/pipe>/d' -e 's/,offload_output//g' \
    -e '/name="Speaker_Out"/s/supportPipes="[^"]*"/supportPipes="primary_output"/' \
    $X/vendor/etc/audio/audio_policy_config.xml > $O/audio_policy_config.xml
# Preload apps: the preload partition's policy directory added to the base layers (Huawei's OEM info would select it).
{ cat $X/system/system/etc/cust/baselayer.cfg; printf '\n# asteroids: the preload partition\n/preload/HBN-AL10/all/cn\n'; } \
    > $O/baselayer.cfg
# The RIL host: the donor's AT vendor library (OpenHarmony's reference one) through asteroids_atd's pty, instead of
# Huawei's Balong transport.
{ sed -e 's|^const.sys.radio.vendorlib.path=.*|const.sys.radio.vendorlib.path=libril_vendor.z.so|' \
      $X/chip_prod/etc/ril_param/common.para
  echo const.telephony.ril.attty.path=/dev/asteroids-at; } > $O/ril-common.para
# The boot animation plays the picture frames (bootpic.zip, made from the donor's boot video) instead of the video,
# which needs the media service.
cat > $O/bootanimation_custom_config.json <<'JSON'
{
	"cust.bootanimation.pics": "/data/local/bootpic.zip",
	"cust.bootanimation.sounds": "",
	"cust.bootanimation.video": ""
}
JSON
# Huawei's Camera app takes its sizes, modes and zoom steps from CameraProfiles.json: ours for these cameras.
python3 $DEVICE/camera/make-camera-profiles.py $X/sys_prod/etc/camera/CameraProfiles.json $O/CameraProfiles.json
ls $O
