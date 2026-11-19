#!/bin/bash
# Builds asteroids.img on the build PC: the asteroids partition in super (EROFS, lz4hc), which M1 init mounts
# read-only at /asteroids, with everything of ours that runs on the phone. The pieces are built into hmos-stage/ and
# laid out at the end:
#   bin/       busybox (with its applet links) and our static tools: pd-mapper, rmtfs, tqftpserv, asteroids_uim,
#              asteroids_gnss, wowlan, slot_success, tap, ssc_probe, asteroids_vibrate
#   boot/      boot/ from the repo: lib.sh, device.conf, prep.sh and prep.d/ (M1 runs it before the handoff),
#              svc/ (the HarmonyOS init services in init/asteroids-helpers.cfg), wake.sh, blackbox.sh, shell.sh
#   init/      the service set (make-m1-services.py: ueventd, hilogd, samgr, hdf, composer/allocator hosts,
#              render_service, bootanimation, desktop-services.txt and ondemand-services.txt) and our configs
#              (asteroids-desktop.cfg, asteroids-helpers.cfg)
#   vendor/    overlaid on the donor's vendor: our composer and allocator VDIs (build-vdi.sh), Turnip as Huawei's
#              Vulkan driver (rebuilt from build-tu-ohos), the A810 firmware, ueventd rules, asteroids.para
#   system/    copied over the root's system: bootanimation (launcher that preloads sheng's Vulkan shim),
#              bootanimation-real (the donor's, patched to draw picture frames full screen), the shim, and
#              sheng's software key stores with the database crypto libraries pointed at them, the code-signing
#              patch, sheng's fallbacks for services that need the TEE, and the HDI publishers
#   etc/       configs prep.d/30-configs.sh binds over the donor's: build-etc.sh's (derived from the donor's own),
#              display_manager_config.xml, hgm_policy_config.xml, socperf_*.xml, security/ (app trust), bootpic.zip
#   android/   what the Android helpers preload: audio/ (PAL server), bluetooth/ (bridge, vendor library),
#              vibrator/ (RichTap server)
#   anco/      the Android container's guest GPU vendor tree and patched libgui (build-anco-adreno.sh, labelled)
#   dhcp/      Alpine's busybox-extras and musl (from m2root.tar.gz) for M1's DHCP server on the USB network
#   stress/    the UI stress test
# Sources from this repo (build/env.sh: common/, platform/qcom/, device/asteroids/, the donor's dir). In WORK
# ($HARMONY_BUILD/m2): bootpic.zip (donor/.../bootpic/make-bootpic.py), m2root.tar.gz (Alpine, for dhcp/),
# build-tu-ohos (the Mesa tree of platform/qcom/gpu/patches/turnip-asteroids.patch) and hmos-stage/ (m1/etc-init and the
# vendor files that aren't built here); security_huks in $HARMONY_BUILD/ohos/src (a sparse checkout: interfaces,
# frameworks, services, utils and build, for the HUKS headers and the software key engine); the donor's images, mounted
# and extracted (build/extract-donor.sh).
set -e
. "$(dirname "$0")/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK  # (cd here: outputs and the stage are relative to it)
N=$HB/ohos/sdk/native
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt/system/system
S=$WORK/hmos-stage
cd $M2

# Display VDIs.
bash $QCOM/display/build-vdi.sh >/dev/null
cp out-vdi/libdisplay_composer_vdi_impl.z.so $S/m2/vendor/lib64/
cp out-vdi/libdisplay_buffer_vdi_impl.z.so $S/m2/vendor/lib64/passthrough/indirect/
cp out-vdi/libgralloc_priv.z.so $S/m2/vendor/lib64/passthrough/
# Turnip (build-tu-ohos, set up by port-turnip-ohos.py) as Huawei's Vulkan driver.
ninja -C build-tu-ohos -j10 src/freedreno/vulkan/libvulkan_freedreno.so >/dev/null
install -m 644 build-tu-ohos/src/freedreno/vulkan/libvulkan_freedreno.so \
    $S/m2/vendor/lib64/passthrough/vulkan.hvgr_v200.so

# Boot animation: sheng's shim, the launcher, and the donor binary patched for full-screen pictures.
[ -f ohos-link/libc++-system.so ] || { sudo -n cp $DONOR/lib64/libc++.so ohos-link/libc++-system.so;
                                       sudo -n chown $(id -u):$(id -g) ohos-link/libc++-system.so; }
mkdir -p out-services $S/m2/system/bin $S/m2/system/lib64
$N/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -std=c++17 -fno-exceptions \
    -fno-rtti -nostdlib++ -include $COMMON/include/platform_cpp_config.h -fuse-ld=lld -shared -fPIC -Wl,-z,defs \
    -Wl,-soname,libsheng_boot_vulkan.so $COMMON/services/bootanimation_vulkan_adapter.cpp ohos-link/libc++-system.so -ldl \
    -o out-services/libsheng_boot_vulkan.so
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld \
    $COMMON/services/bootanimation_launcher.c -o out-services/bootanimation-launcher
sudo -n cat $DONOR/bin/bootanimation > out-services/bootanimation-donor
python3 $DONORDIR/patches/patch-bootanimation-fullpic.py out-services/bootanimation-donor out-services/bootanimation-real
install -m 755 out-services/bootanimation-launcher $S/m2/system/bin/bootanimation
install -m 755 out-services/bootanimation-real $S/m2/system/bin/bootanimation-real
install -m 644 out-services/libsheng_boot_vulkan.so $S/m2/system/lib64/
# sheng's scanout capture: reads the frame on the panel straight from DRM (capture_scanout <out.ppm> [step]).
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld \
    $COMMON/services/capture_scanout.c -o out-services/capture_scanout
install -m 755 out-services/capture_scanout $S/m2/system/bin/capture_scanout
# sheng's render_service launcher: Turnip's diagnostics go to stderr, which render_service otherwise discards.
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld \
    $COMMON/services/render_service_launcher.c -o out-services/render_service_launcher
install -m 755 out-services/render_service_launcher $S/m2/system/bin/render_service_launcher
# Battery (sheng's compat/battery, battery/build-battery.sh): hosts the donor's battery HDI implementation and registers
# it as battery_interface_service, so powermgr's battery service gets the real level and charging state.
bash $COMMON/battery/build-battery.sh >/dev/null
# Wi-Fi kernel side (wifi/): icnss2 with wifi/icnss2-no-dms-mac.patch (the modem, which would supply the MAC, isn't
# running), loaded by prep.sh in place of M1's; the WLAN driver's config from Android's vendor
# (/vendor/firmware/wlan/qca_cld/qca6750/, where the driver looks); an empty /vendor/firmware_mnt for prep.sh to put
# the modem partition's firmware on (the Wi-Fi processor's image is there).
install -D -m 644 $DEVICE/wifi/WCNSS_qcom_cfg.ini $S/m2/vendor/firmware/wlan/qca_cld/qca6750/WCNSS_qcom_cfg.ini
mkdir -p $S/m2/vendor/firmware_mnt
install -m 755 out-battery/native_battery_hdi_publisher $S/m2/system/bin/native_battery_hdi_publisher
# Power (power/build-power.sh): hosts the donor's power HDI and registers it as power_interface_service, which powermgr
# suspends the device through; the donor's power_host, which would normally host it, isn't started.
bash $COMMON/power/build-power.sh >/dev/null
install -m 755 out-power/native_power_hdi_publisher $S/m2/system/bin/native_power_hdi_publisher
# The app freezer's kernel channel (power/kstate.c), loaded by prep.sh before the WLAN driver.
bash $COMMON/power/build-kstate.sh >/dev/null
# Stand-ins for Huawei's log devices (kernel/hwlog/hwlog.c), which the anco container needs; packed with kstate.
bash $COMMON/kernel/hwlog/build-hwlog.sh >/dev/null
install -m 644 bootpic.zip $S/m2/bootpic.zip
# After the handoff, outside HarmonyOS (static, M1's busybox): boot-success marking and the panel capture loop;
# tap injects touches for testing (m2/bin/tap <x> <y>).
for t in $QCOM/services/slot_success $COMMON/services/tap; do
    $N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -static \
        $t.c -o out-services/$(basename $t)
    install -D -m 755 out-services/$(basename $t) $S/m2/bin/$(basename $t)
done
# Audio: the protection-domain mapper (sheng's compat/audio/pd-mapper, Linaro's), which the kernel's audio notifier
# needs before the ADSP's audio domain counts as up and the sound card can register.
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wno-unused-parameter -fuse-ld=lld \
    -static -Iaudio/pd-mapper $QCOM/audio/pd-mapper/*.c -o out-services/pd-mapper
install -D -m 755 out-services/pd-mapper $S/m2/bin/pd-mapper
# Wake-on-WLAN triggers for suspend (wifi/wowlan.c, run by after-boot.sh).
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -static \
    $QCOM/wifi/wowlan.c -o out-services/wowlan
install -D -m 755 out-services/wowlan $S/m2/bin/wowlan
# Modem: Linaro's rmtfs and tqftpserv (modem/build-modem.sh), the SIM activator (asteroids_uim) and the script prep.sh
# boots the modem with; the AT proxy the RIL vendor library talks through (a HarmonyOS service) and the data call tool
# it runs.
bash $QCOM/modem/build-modem.sh $WORK/out-modem >/dev/null
install -m 755 out-modem/rmtfs out-modem/tqftpserv out-modem/asteroids_uim -t $S/m2/bin
install -m 755 out-modem/asteroids_atd out-modem/asteroids_data -t $S/m2/system/bin
# OpenHarmony's reference RIL vendor library, built with both slots and the baseband version (modem/build-ril-vendor.sh),
# over the donor's copy.
bash $QCOM/modem/build-ril-vendor.sh $WORK/out-modem >/dev/null
install -D -m 644 out-modem/libril_vendor.z.so $S/m2/vendor/lib64/libril_vendor.z.so
# The host end of the Android container's virtual RIL (common/anco/vril): Huawei's libril_msgtransfer on the guest's
# socket, with our RIL behind it answering from asteroids_atd's slot 1 state.
bash $COMMON/anco/vril/build-anco-vril.sh $WORK/out-anco-vril >/dev/null
install -m 755 out-anco-vril/anco_vrild -t $S/m2/system/bin
install -m 644 out-anco-vril/libanco_vril.z.so -t $S/m2/system/lib64
# GNSS (gnss/build-gnss.sh): the vendor library location_host's gnss_interface_service dlopens, over the modem's QMI
# LOC, in place of the donor's, which drives Huawei's own GNSS chip. The test tool goes along for checks from a shell.
bash $QCOM/gnss/build-gnss.sh >/dev/null
install -D -m 644 out-gnss/vendorGnssAdapter.so $S/m2/vendor/lib64/hw/vendorGnssAdapter.so
install -m 755 out-gnss/asteroids_gnss -t $S/m2/bin
# The modem's AT channel for the proxy and radio; a modem restart recreates the node, so this is a ueventd rule.
grep -q '^/dev/at_mdm0 ' $S/m2/vendor/etc/ueventd.config ||
    printf '# asteroids: the modem AT channel ($QCOM/modem/asteroids_atd.c).\n/dev/at_mdm0 0660 1001 1001\n' \
        >> $S/m2/vendor/etc/ueventd.config
# Audio: sheng's PAL server and HarmonyOS audio VDI (audio/build-audio.sh). The Bionic server and its Android root
# (audio/start-audio.sh; the root itself is the android partition, android/build-android-helpers.sh) go in m2/audio, outside HarmonyOS; the HarmonyOS side in m2/system.
bash $QCOM/audio/build-audio.sh >/dev/null
install -D -m 644 out-audio/libsheng_pal_server.so out-audio/libsheng_pal_lifecycle.so out-audio/libpal_no_hwbinder.so $DEVICE/audio/backend_conf.xml -t $S/m2/audio
install -m 755 out-audio/pal_tone -t $S/m2/audio
rm -f $S/m2/audio/extract-android.sh
install -m 644 out-audio/libsheng_audio_vdi.so out-audio/libsheng_audio_vdi_loader.so \
    out-audio/libsheng_audio_manager_service.so -t $S/m2/system/lib64
install -m 755 out-audio/native_audio_hdi_publisher out-audio/native_audio_service_launcher \
    out-audio/native_system_audio_check out-audio/native_audio_rec_check -t $S/m2/system/bin
# Bluetooth: sheng's bridge to Qualcomm's HAL and the HCI HDI publisher (bluetooth/build-bluetooth.sh). The bridge and
# start-bluetooth.sh go in m2/bluetooth, outside HarmonyOS, with the vendor library prep.sh binds; the publisher in m2/system.
bash $QCOM/bluetooth/build-bluetooth.sh >/dev/null
install -D -m 644 out-bluetooth/libsheng_bt_bridge.so out-bluetooth/libbt_vendor_sheng.so -t $S/m2/bluetooth
install -m 755 out-bluetooth/bt_hci_test -t $S/m2/bluetooth
install -m 755 out-bluetooth/native_bt_hdi_publisher $S/m2/system/bin/native_bt_hdi_publisher
# The web render processes' sandbox (com.ohos.render) asks for a PID namespace: the kernel has CONFIG_PID_NS now
# (common/kernel/harmony.config), so the donor's appdata-sandbox.json stands (no longer sheng's "net"-only copy).
rm -f $S/m2/system/etc/sandbox/appdata-sandbox.json
# GLES (sheng's design, zink/build-zink.sh): Mesa's Zink on Turnip behind an EGL with OpenHarmony's window platform,
# under the names HarmonyOS's EGL wrapper loads from /vendor/lib64/chipsetsdk (symlinks into passthrough), in place
# of Huawei's Mali driver. Mesa's ES 2 library carries ES 3 too.
bash $QCOM/gpu/zink/build-zink.sh >/dev/null
P=$S/m2/vendor/lib64/passthrough
install -m 644 build-zink-ohos/src/egl/libEGL_impl.so $P/libEGL_impl.so
install -m 644 build-zink-ohos/src/mapi/es1api/libGLESv1_CM_impl.so.1.1.0 $P/libGLESv1_impl.so
install -m 644 build-zink-ohos/src/mapi/es2api/libGLESv2_impl.so.2.0.0 $P/libGLESv2_impl.so
ln -sf libGLESv2_impl.so $P/libGLESv3_impl.so
install -m 644 build-zink-ohos/src/gallium/targets/dri/libgallium-25.0.1.so $P/
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld $QCOM/gpu/zink/native_gles_check.c -lEGL -lGLESv3 \
    -o out-zink/native_gles_check
install -m 755 out-zink/native_gles_check -t $S/m2/system/bin
$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld $QCOM/gpu/zink/native_eglimage_check.c \
    -lEGL -lGLESv3 -lnative_buffer -lnative_window -o out-zink/native_eglimage_check
install -m 755 out-zink/native_eglimage_check -t $S/m2/system/bin
# Sensors (sheng's design, sensors/build-sensors.sh): Android's sscrpcd sensorspd starts the ADSP's sensor framework
# (audio/start-audio.sh), sheng's sensor HDI reads it over QMI and native_sensor_hdi_publisher registers it
# (asteroids_sensor_hdi in asteroids-desktop.cfg); the donor's sensors service starts once it reports ready, a
# parameter sensor_host may set (asteroids-sensor.para.dac).
bash $QCOM/sensors/build-sensors.sh >/dev/null
install -m 755 out-sensors/native_sensor_hdi_publisher out-sensors/native_sensor_check -t $S/m2/system/bin
install -m 644 out-sensors/libsensor_interface_service_3.1.z.so -t $S/m2/system/lib64
install -D -m 755 out-sensors/ssc_probe $S/m2/bin/ssc_probe
echo "bootevent.sheng_sensor_hdi.ready = sensor_host:sensor_host:0775:bool" > $S/m2/vendor/etc/param/asteroids-sensor.para.dac
# render_service's HDR composition wants 10-bit buffers (RGBA_1010102) our allocator and Turnip don't have; the panel
# has no HDR (the composer reports none), but both switches default to on. Off as vendor defaults.
printf 'persist.sys.graphic.hdrimage.enabled=0\npersist.sys.graphic.hdrvideo.enabled=0\n' > $S/m2/vendor/etc/param/asteroids-graphic.para
# Auto-rotate (motion/build-motion.sh): the donor's motion service, which Huawei runs in sensor_host, registered by
# native_motion_hdi_publisher (asteroids_motion_hdi, started with the sensors) over our motion VDI, which turns the
# accelerometer into screen rotation.
bash $QCOM/motion/build-motion.sh >/dev/null
install -m 755 out-motion/native_motion_hdi_publisher -t $S/m2/system/bin
install -m 644 out-motion/libhdi_motion_impl.z.so -t $S/m2/vendor/lib64
# Vibration (vibrator/build-vibrator.sh): the donor's vibrator service, which Huawei runs in vibrator_host, registered
# by native_vibrator_hdi_publisher (asteroids_vibrator_hdi, started with the sensor HDI, so it is up before the sensors
# process's misc device service looks for it) over our vibrator VDI, which plays everything through Android's RichTap
# engine (libaacvibrator, as LineageOS/EvoX's nothing-rt_ics HAL) in the RichTap server, started in the Android root
# by start-richtap.sh (from audio/start-audio.sh); force feedback (haptic_rt) until it is up. The driver (hapticdrv.ko,
# patched; prep.sh swaps it in) initializes the chip from Android's haptic_config.bin (the RAM sine and its layout)
# in /vendor/firmware, which the kernel reads under HarmonyOS's root; the preset streams 100/101_Haptic.bin go along.
# asteroids_vibrate plays from a shell.
bash $DEVICE/vibrator/build-vibrator.sh >/dev/null
install -m 755 out-vibrator/native_vibrator_hdi_publisher -t $S/m2/system/bin
install -m 644 out-vibrator/libhdi_vibrator_impl.z.so -t $S/m2/vendor/lib64
install -m 755 out-vibrator/asteroids_vibrate -t $S/m2/bin
[ -d $HB/fw/vibrator ] || bash $DEVICE/vibrator/extract-firmware.sh
install -m 644 $HB/fw/vibrator/haptic_config.bin $HB/fw/vibrator/100_Haptic.bin $HB/fw/vibrator/101_Haptic.bin \
    -t $S/m2/vendor/firmware
install -D -m 644 out-vibrator/libasteroids_richtap.so -t $S/m2/vibrator
# Camera: HarmonyOS's camera host (camera/build-camera.sh; asteroids_camera_hdi, also the torch) and the bridge's
# Android side (qcom/camera/build-camera.sh: camserver and propd for the camera root, boot/svc/camera.sh; camtool).
bash $QCOM/camera/build-camera.sh >/dev/null
bash $DEVICE/camera/build-camera.sh >/dev/null
install -m 755 out-camera/camera_host -t $S/m2/system/bin
install -m 755 out-camera/camtool -t $S/m2/bin
install -D -m 644 out-camera/libcamserver.so out-camera/libpropd.so -t $S/m2/camera
install -D -m 755 $COMMON/stress/stress.sh $S/m2/stress/stress.sh  # UI stress test (see the script)
# Video: msm_video's decoder and encoder behind our OpenMAX IL core (media/build-media.sh), over the donor's Huawei one
# in the /vendor overlay, and the decode and encode checks. msm_video's firmware (vpu20_2v.mbn, from Android's vendor)
# is already staged in $S/m2/vendor/firmware.
bash $QCOM/media/build-media.sh >/dev/null
install -D -m 644 out-media/libOMX_Core.z.so -t $S/m2/vendor/lib64/passthrough/indirect
install -m 755 out-media/native_video_decode_check out-media/native_video_encode_check out-media/v4l2_probe \
    out-media/v4l2_decode_test out-media/v4l2_encode_test -t $S/m2/system/bin
# SocPerf's interaction boosts (power/patch-socperf.py): the donor's resource and boost files moved from its Kirin to
# these clusters and the GPU (prep.sh binds them over sys_prod's), without the vendor's perfgenius report (no
# backend here), and its executor half switched on, which does the writes as root.
for f in resource boost; do
    sudo -n cat $DONOR/../../sys_prod/etc/soc_perf/socperf_${f}_config.xml > out-services/socperf_${f}_config.xml.donor
done
python3 $DEVICE/power/patch-socperf.py out-services/socperf_resource_config.xml.donor out-services/socperf_boost_config.xml.donor \
    $S/m2/socperf_resource_config.xml $S/m2/socperf_boost_config.xml >/dev/null
mkdir -p $S/m2/vendor/etc/soc_perf $S/m2/vendor/etc/ressched
printf '<?xml version="1.0" encoding="utf-8"?>\n<Configs>\n</Configs>\n' > $S/m2/vendor/etc/soc_perf/socperf_resource_config.xml
printf '%s\n' '<?xml version="1.0" encoding="utf-8"?>' '<pluginlist>' \
    '  <plugin libpath="libsocperf_plugin.z.so" libpathexe="libsocperf_executor_plugin.z.so" switch="1"/>' \
    '</pluginlist>' > $S/m2/vendor/etc/ressched/res_sched_plugin_switch.xml
# Our developer signing identity added to the donor's HAP trust config (prep.sh binds these over
# /system/etc/security/), so apps signed with tools/asteroids-signing/ install. sudo: donor files are root-owned.
mkdir -p $S/m2/security
# The trusted root: the one gen-pki.sh made next to the signing keys (else the copy in the repo).
ROOTCA=$HB/tools/asteroids-signing/rootCA.cer; [ -f $ROOTCA ] || ROOTCA=$COMMON/signing/asteroids-root.cer
[ -f $ROOTCA ] || { echo "no signing root: run common/signing/gen-pki.sh in $HB/tools/asteroids-signing" >&2; exit 1; }
sudo -n python3 $COMMON/signing/merge-trust.py $DONOR/etc/security $S/m2/security $ROOTCA
sudo -n chown -R $(id -u):$(id -g) $S/m2/security

# Desktop services: the donor's configs for the framework (foundation and what it needs), "critical" removed.
# init.sched.cfg has no services: it makes the cpuset/cpuctl groups resource_schedule_service moves processes into
# (top-app, foreground, background on cores 0-3...); without it every task stayed in the root group, on all cores.
# Its Kirin-only writes (DDR and GPU devfreq nodes, cgroup_id, cpu.top_task) just fail here.
sudo -n python3 $REPO/build/make-services.py $DONOR/etc/init $S/m1/etc-init $(cat $DONORDIR/desktop-services.txt) >/dev/null
# Every donor config whose services are all on-demand (ondemand-services.txt): they start only when something asks
# for them, and without a config the caller waits out a load timeout ("process:... not exist"). el5_filekey_manager is
# left out on purpose (see the EL5 fallback below).
sudo -n python3 $REPO/build/make-services.py $DONOR/etc/init $S/m1/etc-init $(cat $DONORDIR/ondemand-services.txt) >/dev/null
sudo -n chown -R $(id -u):$(id -g) $S/m1/etc-init
# HDF hosts: the display pair, the donor's user_auth_host, which loads sheng's software user-auth driver below,
# pin_auth_host (the donor's PIN driver over our software PIN backend, below), and
# the Wi-Fi hosts (wifi_host for the WLAN HDI, wpa_host and hostapd_host on demand), the RIL host
# (riladapter_host: the telephony HDI over the modem's AT channel) and location_host (on demand, when locationhub asks
# for GNSS; its gnss_interface_service loads our vendorGnssAdapter.so, below).
sudo -n python3 $REPO/build/make-services.py $DONOR/etc/init $S/m1/etc-init \
    ../../../../chip_prod/etc/init/hdf_devhost.cfg:composer_host,allocator_host,user_auth_host,pin_auth_host,wifi_host,wpa_host,hostapd_host,riladapter_host,location_host \
    >/dev/null
sudo -n chown -R $(id -u):$(id -g) $S/m1/etc-init
# Software user auth (sheng's, useriam/build-useriam.sh): useriam can answer "no credential enrolled", so the lock
# screen unlocks. The libraries replace the donor's TEE-backed ones under their own names; useriam runs only the core
# user-auth abilities (useriam/useriam.json, as sheng does) and PIN auth, without face, fingerprint or companion auth.
bash $COMMON/useriam/build-useriam.sh >/dev/null
for f in libsheng_userauth_4_0.so libuser_auth_interface_service_4.1.z.so libuser_auth_driver.z.so; do
    install -m 644 out-useriam/$f $S/m2/vendor/lib64/
done
# Software PIN (pinauth/build-pinauth.sh): OpenHarmony's PIN backend in place of the donor's TEE-backed one, so a
# lock-screen PIN can be set; PINs are kept in /data/service/el1/public/pinauth (made in asteroids-desktop.cfg).
bash $COMMON/pinauth/build-pinauth.sh >/dev/null
install -m 644 out-pinauth/libpin_auth_interface_service_3.0.z.so $S/m2/vendor/lib64/
install -D -m 644 $COMMON/useriam/useriam.json $S/m2/system/profile/useriam.json
# Once SceneBoard is up: boot completed (powermgr ignores wake requests until then), the boot animation stopped, and
# the display woken through power-shell run as a service, which gets the native token a shell lacks.
install -m 644 $COMMON/init/asteroids-desktop.cfg $S/m1/etc-init/asteroids-desktop.cfg
install -m 644 $DEVICE/display/display_manager_config.xml $S/m2/display_manager_config.xml
# The refresh-rate policy with the 120 Hz setting fixed and LTPO off, as sheng (display/patch-hgm-policy.py).
sudo -n cat $DONOR/../../sys_prod/etc/graphic/hgm_policy_config.xml > out-services/hgm_policy_config.xml.donor
python3 $DEVICE/display/patch-hgm-policy.py out-services/hgm_policy_config.xml.donor $S/m2/hgm_policy_config.xml >/dev/null
# No TEE, so no working HUKS: sheng's software key stores. accountmgr preloads the account-file HMAC; the database
# crypto libraries are pointed at libdbkeys.so (framework/patch-huks-imports.py).
H=$HB/ohos/src/security_huks/interfaces/inner_api/huks_standard/main/include
L=$WORK/ohos-link
for f in chipset-sdk-sp/libcrypto_openssl.z.so chipset-sdk/libhukssdk.z.so; do
    [ -f $L/$(basename $f) ] || sudo -n cat $DONOR/lib64/$f > $L/$(basename $f)
done
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -I$H \
    -std=gnu11 -Wno-misleading-indentation -shared -fPIC -Wl,-z,relro,-z,now"
mkdir -p out-framework
$CC -Wl,--version-script=$COMMON/framework/account_hmac.map $COMMON/framework/account_hmac.c $L/libcrypto_openssl.z.so \
    -Wl,--allow-shlib-undefined -o out-framework/libsheng_account_hmac.so
$CC -Wl,--version-script=$COMMON/framework/db_key.map -Wl,-soname,libdbkeys.so $COMMON/framework/db_key.c $L/libhukssdk.z.so \
    $L/libcrypto_openssl.z.so -Wl,--allow-shlib-undefined -o out-framework/libdbkeys.so
while read -r lib prefix names; do
    sudo -n cat $DONOR/lib64/$lib > out-framework/$lib.donor
    python3 $DONORDIR/patches/patch-huks-imports.py out-framework/$lib.donor out-framework/$lib $prefix $names >/dev/null
done <<'LIBS'
librelational_store_crypt.z.so S Abort Finish GenerateKey GenerateRandom Init KeyExist Update
libkv_store_crypt.z.so S Decrypt Encrypt GenerateKey GenerateRandom KeyExist
libdistributeddatasvcfwk.z.so D Decrypt Encrypt GenerateKey GenerateRandom KeyExist
LIBS
# No code-signing kernel support: skip fs-verity enabling at install (framework/patch-code-sign.py).
sudo -n cat $DONOR/lib64/libcode_sign_utils.z.so > out-framework/libcode_sign_utils.z.so.donor
python3 $DONORDIR/patches/patch-code-sign.py out-framework/libcode_sign_utils.z.so.donor out-framework/libcode_sign_utils.z.so \
    >/dev/null
# The debug-install counterpart (framework/patch-key-enable.py): the profile-key enrollment opens /dev/code_sign, which
# our kernel lacks, so it is made a no-op success. Lets apps we sign with a debug profile (m2/signing) install.
sudo -n cat $DONOR/lib64/libkey_enable.z.so > out-framework/libkey_enable.z.so.donor
python3 $DONORDIR/patches/patch-key-enable.py out-framework/libkey_enable.z.so.donor out-framework/libkey_enable.z.so >/dev/null
# sheng's client-library patches, found again in our donor (framework/patch-insns.py): SceneBoard's UI thread no longer
# blocks on services that can't come up, and the time service skips a clock Linux lacks. Plus ours: powermgr hears about
# every touch while the screen is on, memmgr starts without Huawei's memcg extensions, and msdp stops retrying a
# missing HDI past its watchdog, and the Android container manager serves our re-signed DroiTong. (sheng's rs-commit-sync, also in patch-insns.py, isn't needed: the kernel reports
# one-way binder senders, common/kernel/binder-oneway-pid.py.)
for f in platformsdk/libaudio_policy_client.z.so platformsdk/libsensor_client.z.so \
         libsps_privacy_indicator_service_intf.z.so platformsdk/libwifi_sdk.z.so \
         platformsdk/libnet_tether_manager_if.z.so platformsdk/libcli_tool_client.z.so libpowermgrservice.z.so \
         libtime_system_ability.z.so platformsdk/libsensor_utils.z.so libmemmgrservice.z.so libmovement_ar.z.so \
         libhmos_fusion_manager.z.so; do
    sudo -n cat $DONOR/lib64/$f > out-framework/$(basename $f).donor
    python3 $DONORDIR/patches/patch-insns.py $(basename $f) out-framework/$(basename $f).donor out-framework/$(basename $f) >/dev/null
    install -D -m 644 out-framework/$(basename $f) $S/m2/system/lib64/$f
done
# The composer service's present-fence leak (sheng's present-fence fix, in patch-insns.py), in the /vendor overlay.
sudo -n cat $DONOR/../../vendor/lib64/libdisplay_composer_service_1.5.z.so > out-framework/libdisplay_composer_service_1.5.z.so.donor
python3 $DONORDIR/patches/patch-insns.py libdisplay_composer_service_1.5.z.so out-framework/libdisplay_composer_service_1.5.z.so.donor \
    out-framework/libdisplay_composer_service_1.5.z.so >/dev/null
install -m 644 out-framework/libdisplay_composer_service_1.5.z.so $S/m2/vendor/lib64/
# A software HUKS: OpenHarmony's key engine in place of the HDI's TEE client (framework/build-huks-engine.sh), in the
# /vendor overlay. huks_service then publishes SA 3510, so apps' and services' HUKS keys work (no hardware protection).
bash $COMMON/framework/build-huks-engine.sh out-framework/libhuks_ca_hdi.z.so >/dev/null
install -m 644 out-framework/libhuks_ca_hdi.z.so $S/m2/vendor/lib64/passthrough/indirect/
# sheng's fallbacks for services we can't run. EL5 (the per-app file-key service for the strongest encryption class)
# needs the TEE: installs gets "unavailable" at once instead of a multi-second load timeout per app. The SceneBoard
# ones (optional SAs listed in /system/etc/sceneboard-optional-sa.list, and Wi-Fi) are built and staged but not
# preloaded anywhere yet; they filter on SceneBoard's process name, for when it stalls on those services.
$CC -std=gnu11 $COMMON/framework/el5_unavailable.c -o out-framework/libsheng_el5_unavailable.so
$CC -std=gnu11 -ldl $COMMON/framework/wifi_ui_unavailable.c -o out-framework/libsheng_wifi_ui_unavailable.so
$N/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld -std=c++17 \
    -fno-exceptions -fno-rtti -nostdlib++ -shared -fPIC -Wl,-z,relro,-z,now $COMMON/framework/sceneboard_optional_sa.cpp -ldl \
    -o out-framework/libsheng_sceneboard_optional_sa.so
for f in libsheng_account_hmac.so libdbkeys.so librelational_store_crypt.z.so libkv_store_crypt.z.so \
         libdistributeddatasvcfwk.z.so libcode_sign_utils.z.so libkey_enable.z.so libsheng_el5_unavailable.so \
         libsheng_wifi_ui_unavailable.so libsheng_sceneboard_optional_sa.so; do
    install -m 644 out-framework/$f $S/m2/system/lib64/
done
# sheng's Wi-Fi category preload (wifi/sheng, his compat/wifi): answers Huawei's private CMD_WIFI_CATEGORY ioctl with the
# HE/EHT capabilities read over nl80211, so the status bar's Wi-Fi icon shows Wi-Fi 6/7. Preloaded into wifi_host below.
# (the SDK's nl80211.h predates Wi-Fi 7: the kernel's NL80211_BAND_IFTYPE_ATTR_EHT_CAP_PHY is 9)
# Also the hotspot's country code (wifi/sheng/country_code_v134.c): the donor's HAL sends it as a command qcacld
# doesn't know, and wifi_manager won't start the hotspot when that fails.
$CC -std=gnu11 -DNL80211_BAND_IFTYPE_ATTR_EHT_CAP_PHY=9 -shared -fPIC -Wl,-z,defs $QCOM/wifi/sheng/wifi_category_ioctl_v134.c $QCOM/wifi/sheng/wifi_caps_v134.c \
    $QCOM/wifi/sheng/country_code_v134.c -o out-framework/libsheng_wifi_category.so
install -m 644 out-framework/libsheng_wifi_category.so $S/m2/system/lib64/
python3 - $S/m1/etc-init <<'PY'
import json, sys
# The sensors service waits for the sensor HDI (asteroids-desktop.cfg starts it when the HDI reports ready).
sensors = sys.argv[1] + "/sensors_musl.cfg"
cfg = json.load(open(sensors))
for svc in cfg["services"]:
    if svc["name"] == "sensors":
        svc.update({"disabled": 1, "start-mode": "condition"})
json.dump(cfg, open(sensors, "w"), indent=4)
graphic = sys.argv[1] + "/graphic.cfg"
cfg = json.load(open(graphic))
for svc in cfg["services"]:
    if svc["name"] == "render_service":
        svc["path"] = ["/system/bin/render_service_launcher"]
json.dump(cfg, open(graphic, "w"), indent=4)
# The app freezer (in resource_schedule_service, uid ressched) moves other apps' processes into /dev/frz's cgroups.
# Huawei's kernel allows that; Linux's v1 cgroups want root, the same uid or CAP_SYS_NICE.
rss = sys.argv[1] + "/resource_schedule_service.cfg"
cfg = json.load(open(rss))
for svc in cfg["services"]:
    if svc["name"] == "resource_schedule_service":
        svc["caps"] = sorted(set(svc.get("caps", [])) | {"SYS_NICE"})
json.dump(cfg, open(rss, "w"), indent=4)
for cfg_name, service, preload in (("accountmgr.cfg", None, "libsheng_account_hmac.so"),
                                   ("installs.cfg", None, "libsheng_el5_unavailable.so"),
                                   ("hdf_devhost.cfg", "wifi_host", "libsheng_wifi_category.so")):
    path = sys.argv[1] + "/" + cfg_name
    cfg = json.load(open(path))
    for svc in cfg["services"]:
        if service in (None, svc["name"]):
            svc["env"] = [{"name": "LD_PRELOAD", "value": "/system/lib64/" + preload}]
    json.dump(cfg, open(path, "w"), indent=4)
PY

# The partition's tree, laid out from the stage and the repo (only what is named here: the stage keeps old files).
A=$WORK/asteroids-root
rm -rf $A && mkdir -p $A/bin $A/init $A/android $A/dhcp/bin $A/dhcp/lib $A/dhcp/tmp
install -m 755 $HB/tools/busybox/x/bin/busybox.static $A/bin/busybox
# Its applets as links beside it (the scripts run with PATH=/asteroids/bin), from the binary's applet name table
# ("[\0[[\0acpid\0..."; the arm64 binary can't run here to list them).
python3 - $A/bin/busybox <<'PY'
import os, sys
data = open(sys.argv[1], "rb").read()
i = data.index(b"[\0[[\0")
names = data[i:data.index(b"\0\0", i)].split(b"\0")
for n in names:
    os.symlink("busybox", os.path.join(os.path.dirname(sys.argv[1]), n.decode()))
print(len(names), "busybox applets")
PY
cp -a $S/m2/bin/. $A/bin/
# boot/: each layer's boot/ (common, then the platform's, then the device's), its init/*.cfg into init/.
mkdir -p $A/boot
for l in $COMMON $QCOM $DEVICE; do [ -d $l/boot ] && cp -a $l/boot/. $A/boot/; done
cp $DEVICE/device.conf $A/boot/
mv $A/boot/init/*.cfg $A/init/ && rmdir $A/boot/init
cp -a $S/m1/etc-init/. $A/init/
cp -a $S/m2/vendor $S/m2/system $S/m2/stress $A/
# anco/: the Android container's GPU vendor tree and patched libgui (common/apps/build-anco-adreno.sh), carrying the
# guest's SELinux labels as xattrs (the guest's init won't start a service whose binary has no exec type).
AN=$HB/fw/anco-adreno
if [ -f $AN/labels.txt ]; then
    mkdir -p $A/anco
    cp -a $AN/vendor $AN/system $A/anco/
    (cd $A/anco && while read l p; do sudo -n setfattr -h -n security.selinux -v "$l" "$p"; done < $AN/labels.txt)
fi
bash $REPO/build/build-etc.sh $A >/dev/null
cp -a $S/m2/security $A/etc/
install -m 644 bootpic.zip $DEVICE/display/display_manager_config.xml -t $A/etc
install -m 644 $S/m2/hgm_policy_config.xml $S/m2/socperf_resource_config.xml $S/m2/socperf_boost_config.xml -t $A/etc
mkdir -p $A/android/audio $A/android/bluetooth $A/android/vibrator $A/android/camera
cp -a $S/m2/audio/*.so $S/m2/audio/backend_conf.xml $S/m2/audio/pal_tone $A/android/audio/
cp -a $S/m2/bluetooth/*.so $S/m2/bluetooth/bt_hci_test $A/android/bluetooth/
cp -a $S/m2/vibrator/*.so $A/android/vibrator/
cp -a $S/m2/camera/*.so $A/android/camera/
tar -xzf m2root.tar.gz -C $A/dhcp --strip-components=1 m2root/bin/busybox-extras m2root/lib/ld-musl-aarch64.so.1
ln -s ld-musl-aarch64.so.1 $A/dhcp/lib/libc.musl-aarch64.so.1
$HB/tools/erofs-utils/bin/mkfs.erofs -zlz4hc --all-root -T0 --quiet asteroids.img.new $A
mv asteroids.img.new asteroids.img
ls -l asteroids.img
echo BUNDLE-OK
