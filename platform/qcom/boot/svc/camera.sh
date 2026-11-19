# The camera (HarmonyOS init service asteroids_camera, on asteroids.android.ready): Qualcomm's CamX HAL behind camserver
# (platform/qcom/camera/bridge/camserver.c), which HarmonyOS's camera host (device/asteroids/camera/camera_host.cpp)
# reaches over /dev/unix/socket/asteroids.camera.
#
# CamX needs what Android's camera provider has around it, which the other helpers' root (/android, android-root.sh)
# doesn't: Android's linker namespaces, a binder context with the service managers, the gralloc allocator and the perf
# HAL, and a property service. Giving /android those would change how the audio helpers load, so the camera gets a
# root of its own in /android/camera, made from the same partition:
#   - system, system_ext, vendor, odm and the two APEXes bound from /android (without android-root.sh's vendor/etc
#     overlay); the DSP modules on vendor/dsp, persist (read-only), our libraries (/asteroids/android/camera) on
#     vendor/lib64/asteroids, where the vendor namespace may load them; /data from /data/asteroids/camera
#   - its own /dev: the device nodes copied, a binderfs instance of its own (an Android binder context apart from
#     HarmonyOS's), its own sockets and property area; selinuxfs (servicemanager reads SELinux's status page)
# Then the services (run below): propd (platform/qcom/android/propd.c; the property area from the build.prop files),
# linkerconfig once the properties are there (it reads the VNDK version from them), servicemanager, hwservicemanager,
# the QTI gralloc allocator, the QTI perf HAL and camserver, which starts and stops the compute DSP as the camera opens
# and closes (camserver is a preload of a vendor program, toybox's sleep, so the vendor section of the linker config
# applies to it). If camserver exits, all of them are started again (lib.sh's respawn). CamX boots the ICP (the
# camera's image processing controller) once, when the HAL starts, with firmware from $D/firmware (below).
# camserver offers the back camera as its main, ultrawide and tele lenses (HAL cameras 0, 2 and 3: it hands over
# between them as the zoom goes) and the front one (1).
# A configuration only shows camera files missing from the partition in CamX's own log: with
# /data/local/tmp/camtest/logdw, "logdw /android/camera/dev/socket 5" in init's namespace collects it.
# Logs: /data/local/tmp/camserver.txt and camera-<service>.txt; the camera test root (platform/qcom/camera/test) is the
# same setup over Android's full images.
TAG=camera
. /asteroids/boot/lib.sh
A=/android
C=$A/camera
T=/data/local/tmp
D=/data/asteroids/camera
LP=/vendor/lib64:/vendor/lib64/hw:/system/lib64:/apex/com.android.vndk.v34/lib64:/apex/com.android.runtime/lib64:/apex/com.android.runtime/lib64/bionic
CAMERA_HOST_UID=3028

root() {
    mountpoint -q $C && return 0
    mount -t tmpfs -o size=8m,mode=755 camroot $C || return 1
    for d in system system_ext vendor odm apex/com.android.runtime apex/com.android.vndk.v34 dev proc sys data \
        linkerconfig mnt/vendor/persist hmos-socket; do mkdir -p $C/$d; done
    for d in system system_ext vendor odm apex/com.android.runtime apex/com.android.vndk.v34; do
        mount --bind $A/$d $C/$d || return 1
    done
    ln -s system/bin $C/bin
    mount --bind /sys $C/sys && mount -t proc proc $C/proc || return 1
    mount -t selinuxfs selinuxfs $C/sys/fs/selinux 2>/dev/null
    mkdir -p $D/vendor/camera $D/vendor/gaming
    mount --bind $D $C/data || return 1
    mount -t ext4 -o ro,norecovery /dev/$(part $DSP_PART$SLOT) $C/vendor/dsp || log "$DSP_PART$SLOT not mounted"
    mount -t ext4 -o ro,noload /dev/$(part persist) $C/mnt/vendor/persist || log "persist not mounted"
    mount --bind /asteroids/android/camera $C/vendor/lib64/asteroids || return 1
    mount --bind /dev/unix/socket $C/hmos-socket || return 1
    mount -t tmpfs -o size=4m,mode=755 camdev $C/dev || return 1
    for e in /dev/*; do
        case ${e#/dev/} in binderfs|pts|shm|socket|unix|__parameters__|__properties__) continue;; esac
        cp -a $e $C/dev/ 2>/dev/null
    done
    mkdir -p $C/dev/binderfs $C/dev/socket $C/dev/pts
    mount -t binder binder $C/dev/binderfs || return 1
    # The ICP's firmware where the kernel finds it: it resolves the firmware path in the boot's own mount namespace
    # (M1's root), where /data is the same as here but /android isn't. First in the path: as the last of its five
    # entries it wasn't reached. The directory holds only the ICP's files, so nothing else is found there first.
    mkdir -p $D/firmware && cp -u $A/vendor/firmware/CAMERA_ICP* $D/firmware/ || log "no ICP firmware"
    P=/sys/module/firmware_class/parameters/path
    grep -q "^$D/firmware," $P || echo -n "$D/firmware,$(cat $P)" > $P
}

# Every process in the camera root.
stop_all() {
    for p in /proc/[0-9]*; do
        [ "$(readlink $p/root)" = $C ] && kill -9 ${p#/proc/} 2>/dev/null
    done
    sleep 0.5
}

# start <name> <command...>: a daemon in the root, its output in $T/camera-<name>.txt.
start() {
    n=$1; shift
    setsid env LD_LIBRARY_PATH=$LP "$@" > $T/camera-$n.txt 2>&1 < /dev/null &
}

run() {
    stop_all
    rm -rf $C/dev/__properties__ $C/dev/socket/property_service
    props=
    # Not system_ext's: with it the property area fills up.
    for f in /system/build.prop /vendor/build.prop /odm/etc/build.prop; do
        [ -f $C$f ] && props="$props $f"
    done
    # libvintf picks the device manifest by the SKU (vendor/etc/vintf/manifest_volcano.xml).
    start propd env PROPD="$props ro.boot.hardware=qcom ro.boot.product.vendor.sku=volcano" \
        LD_PRELOAD=/vendor/lib64/asteroids/libpropd.so chroot $C /apex/com.android.runtime/bin/linker64 /vendor/bin/agmplay -h
    until_ 50 test -S $C/dev/socket/property_service || { log "propd didn't start"; return 1; }
    if [ ! -s $C/linkerconfig/ld.config.txt ]; then
        cat > $C/apex/apex-info-list.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<apex-info-list>
    <apex-info moduleName="com.android.runtime" modulePath="/system/apex/com.android.runtime.apex" preinstalledModulePath="/system/apex/com.android.runtime.apex" versionCode="1" versionName="" isFactory="true" isActive="true" lastUpdateMillis="0" provideSharedApexLibs="false" partition="SYSTEM"/>
    <apex-info moduleName="com.android.vndk.v34" modulePath="/system_ext/apex/com.android.vndk.v34.apex" preinstalledModulePath="/system_ext/apex/com.android.vndk.v34.apex" versionCode="1" versionName="" isFactory="true" isActive="true" lastUpdateMillis="0" provideSharedApexLibs="false" partition="SYSTEM_EXT"/>
</apex-info-list>
EOF
        env LD_LIBRARY_PATH=$LP chroot $C /apex/com.android.runtime/bin/linker64 \
            /apex/com.android.runtime/bin/linkerconfig --target /linkerconfig > $T/camera-linkerconfig.txt 2>&1
        grep -q "namespace.vndk" $C/linkerconfig/ld.config.txt || { log "linkerconfig without the VNDK"; return 1; }
    fi
    for s in /system/bin/servicemanager /system_ext/bin/hwservicemanager \
        /vendor/bin/hw/vendor.qti.hardware.display.allocator-service /vendor/bin/vendor.qti.qspmhal-service; do
        start $(basename $s) chroot $C /apex/com.android.runtime/bin/linker64 $s
        sleep 0.5
    done
    [ -f $T/camserver.txt ] && mv $T/camserver.txt $T/camserver.old.txt
    setsid env LD_PRELOAD=/vendor/lib64/asteroids/libcamserver.so CAMSERVER_UID=$CAMERA_HOST_UID CAMSERVER_CAMERAS=0+2+3,1 \
        chroot $C /apex/com.android.runtime/bin/linker64 /vendor/bin/toybox_vendor sleep 2147483647 \
        > $T/camserver.txt 2>&1 < /dev/null &
    p=$!
    until_ 300 sh -c "grep -q '^CAMSERVER_READY=1' $T/camserver.txt || ! [ -d /proc/$p ]"
    grep -q '^CAMSERVER_READY=1' $T/camserver.txt ||
        { log "camserver didn't start: $(tail -2 $T/camserver.txt | tr '\n' ' ')"; return 1; }
    log "camserver ready (pid $p): $(grep -c '^CAMSERVER camera [0-9]* (HAL' $T/camserver.txt) cameras"
    wait $p
}

root || fail "camera root not made"
respawn run
