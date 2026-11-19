# The CamX test root on the phone (README): Android's full slot b system, vendor and odm with the services the camera
# HAL needs around it, in an Android binder context of its own, and the probe that lists the cameras.
#   sh camx-test.sh root     /camroot: the images, the APEXes from /android, its own /dev with a new binderfs instance,
#                            selinuxfs, CamX's override settings, Android's linker namespaces, the CDSP started,
#                            the ICP firmware on the firmware path
#   sh camx-test.sh stack    logdw, propd, servicemanager, hwservicemanager, the gralloc allocator, the perf HAL,
#                            tombstoned (crash dumps in $T/data/tombstones; backtraces for stacks)
#   sh camx-test.sh stacks <pid>   every thread's backtrace of a process in the root (a hang)
#   sh camx-test.sh probe [timeout]
#   sh camx-test.sh capture [camera]   a still with the flash as a torch: $T/data/capture.jpg and preview.ppm
#   sh camx-test.sh server   the camera bridge's server (bridge/camserver.c) on HarmonyOS's asteroids.camera socket,
#                            log in $T/camserver.txt; try it with $T/camtool (list, shoot)
#   sh camx-test.sh stop     everything above undone: the root's processes killed, its mounts gone, the firmware path
#                            back, the CDSP stopped (the boot leaves it off; it costs power)
# Run from M1's shell (./port sh). Everything lives in $T; the logs go to $T/logcat.txt.
export PATH=/asteroids/bin
T=/data/local/tmp/camtest
C=/camroot
LP=/vendor/lib64:/vendor/lib64/hw:/system/lib64:/apex/com.android.vndk.v34/lib64:/apex/com.android.runtime/lib64:/apex/com.android.runtime/lib64/bionic
part() { for u in /sys/class/block/*/uevent; do grep -q "^PARTNAME=$1\$" $u && { basename $(dirname $u); return; }; done; }
in_root() { env LD_LIBRARY_PATH=$LP "$@"; }

root() {
  if ! grep -q " $C tmpfs" /proc/mounts; then
    mkdir -p $C && mount -t tmpfs -o size=16m camroot $C
    for d in system system_ext vendor odm sysimg apex/com.android.runtime apex/com.android.vndk.v34 dev proc sys data \
        m2cam linkerconfig mnt/vendor/persist; do mkdir -p $C/$d; done
    mount -t erofs -o loop,ro $T/system.img $C/sysimg && mount --bind $C/sysimg/system $C/system
    mount -t erofs -o loop,ro $T/vendor.img $C/vendor
    mount -t erofs -o loop,ro $T/odm.img $C/odm
    mount --bind /proc/1/root/android/apex/com.android.runtime $C/apex/com.android.runtime
    mount --bind /proc/1/root/android/apex/com.android.vndk.v34 $C/apex/com.android.vndk.v34
    mount --bind /sys $C/sys; mount -t proc proc $C/proc
    mkdir -p $T/data/vendor/camera $T/data/vendor/gaming $T/m2cam $T/system_ext
    mount --bind $T/data $C/data; mount --bind $T/m2cam $C/m2cam
    tar -xzf $T/system_ext.tgz -C $T/system_ext && mount --bind $T/system_ext $C/system_ext
    mount -t ext4 -o ro,norecovery /dev/$(part dsp_b) $C/vendor/dsp
    mount -t ext4 -o ro,noload /dev/$(part persist) $C/mnt/vendor/persist
    ln -s system/bin $C/bin
  fi
  # Its own /dev: the device nodes copied, a binderfs instance of its own (an Android binder context apart from
  # HarmonyOS's), sockets and the property area its own.
  if ! grep -q "^camdev $C/dev " /proc/mounts; then
    mount -t tmpfs -o size=4m,mode=755 camdev $C/dev
    for e in /dev/*; do
      case ${e#/dev/} in binderfs|pts|shm|socket|unix|__parameters__|__properties__) continue;; esac
      cp -a $e $C/dev/ 2>/dev/null
    done
    mkdir -p $C/dev/binderfs $C/dev/socket $C/dev/pts
    mount -t binder binder $C/dev/binderfs
  fi
  # servicemanager wants SELinux's status page (the policy is permissive).
  grep -q "^selinuxfs $C/sys/fs/selinux " /proc/mounts || mount -t selinuxfs selinuxfs $C/sys/fs/selinux
  # CamX's override settings over the read-only vendor: no non-camera sensor service (its FastRPC channel to the sensor
  # PD blocks; EIS gyro data comes from it).
  mkdir -p $T/camera-etc/upper $T/camera-etc/work
  grep -q "^overlay $C/vendor/etc/camera " /proc/mounts || mount -t overlay overlay \
      -o lowerdir=$C/vendor/etc/camera,upperdir=$T/camera-etc/upper,workdir=$T/camera-etc/work $C/vendor/etc/camera
  # And CHI's own stream buffer manager off (it asks for buffers on top of the request's and keeps one per frame), and
  # MFNR, whose JPEG path wants a queue of earlier full-size frames.
  printf "halBufferMgrMode=0\noverrideEnableMFNR=FALSE\n" > $C/vendor/etc/camera/camxoverridesettings.txt
  # The compute DSP, which the boot leaves offline: CamX's motion estimation (libhme) runs there and crashes the
  # pipeline without it.
  for r in /sys/class/remoteproc/*; do
    grep -q cdsp $r/name && [ "$(cat $r/state)" = offline ] && echo start > $r/state
  done
  # The ICP's firmware, where the kernel looks (it resolves the path in init's root, where /camroot isn't).
  mkdir -p $T/fw && cp -u $C/vendor/firmware/CAMERA_ICP* $T/fw/
  P=/sys/module/firmware_class/parameters/path
  grep -q "$T/fw" $P || echo -n "$T/fw,$(cat $P)" > $P
  # The vendor namespace loads only from /odm and /vendor: the probe goes in the vendor overlay.
  mkdir -p $C/vendor/etc/camera/asteroids
  echo "root ready"
}

linkerconfig() {
  # Android's linker namespaces (linkerconfig, as Android's init runs it): vendor code gets the VNDK 34 libraries it
  # was built against and LLNDK from the system namespace; system code gets its own. In one flat namespace the
  # vendor's allocator interface V1 met libui's V2 and CamX's buffer objects came out corrupt.
  # linkerconfig reads the VNDK version from the properties: it runs once propd is up (stack), or the vendor section
  # comes out without the VNDK.
  [ -s $C/linkerconfig/ld.config.txt ] && return
  if :; then
    cat > $C/apex/apex-info-list.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<apex-info-list>
    <apex-info moduleName="com.android.runtime" modulePath="/system/apex/com.android.runtime.apex" preinstalledModulePath="/system/apex/com.android.runtime.apex" versionCode="1" versionName="" isFactory="true" isActive="true" lastUpdateMillis="0" provideSharedApexLibs="false" partition="SYSTEM"/>
    <apex-info moduleName="com.android.vndk.v34" modulePath="/system_ext/apex/com.android.vndk.v34.apex" preinstalledModulePath="/system_ext/apex/com.android.vndk.v34.apex" versionCode="1" versionName="" isFactory="true" isActive="true" lastUpdateMillis="0" provideSharedApexLibs="false" partition="SYSTEM_EXT"/>
</apex-info-list>
EOF
    in_root chroot $C /apex/com.android.runtime/bin/linker64 /apex/com.android.runtime/bin/linkerconfig --target /linkerconfig
  fi
}

stack() {
  pkill -f "$T/logdw"; sleep 0.3
  setsid $T/logdw $C/dev/socket ${LOGPRIO:-3} > $T/logcat.txt 2>&1 &
  sleep 0.5
  # The services map the property area, so a new propd means new services.
  if ! kill -0 $(cat $T/propd.pid 2>/dev/null) 2>/dev/null; then
    for s in servicemanager hwservicemanager allocator-service qspmhal-service; do pkill -f "linker64 .*$s"; done
    props=""
    for f in /system/build.prop /vendor/build.prop /odm/etc/build.prop; do [ -f $C$f ] && props="$props $f"; done
    rm -rf $C/dev/__properties__
    # In the vendor overlay, where the vendor namespace may load it from once the linker config is there.
    cp $T/m2cam/libpropd.so $C/vendor/etc/camera/asteroids/
    # libvintf picks the device manifest by the SKU (vendor/etc/vintf/manifest_volcano.xml).
    setsid env LD_LIBRARY_PATH=$LP PROPD="$props ro.boot.hardware=qcom ro.boot.product.vendor.sku=volcano" \
        LD_PRELOAD=/vendor/etc/camera/asteroids/libpropd.so chroot $C /apex/com.android.runtime/bin/linker64 /vendor/bin/agmplay -h \
        > $T/propd.out 2>&1 &
    echo $! > $T/propd.pid
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -S $C/dev/socket/property_service ] && break; sleep 0.5; done
  fi
  linkerconfig
  for s in /system/bin/servicemanager /system_ext/bin/hwservicemanager \
      /vendor/bin/hw/vendor.qti.hardware.display.allocator-service /vendor/bin/vendor.qti.qspmhal-service; do
    pgrep -f "linker64 $s" >/dev/null && continue
    setsid env LD_LIBRARY_PATH=$LP chroot $C /apex/com.android.runtime/bin/linker64 $s > $T/$(basename $s).out 2>&1 &
    sleep 1
  done
  mkdir -p $T/data/tombstones $T/data/anr
  pgrep -f "linker64 /system/bin/tombstoned" >/dev/null || setsid env LD_LIBRARY_PATH=$LP $T/socketexec $C \
      tombstoned_crash:seqpacket tombstoned_intercept:seqpacket tombstoned_java_trace:seqpacket -- \
      /apex/com.android.runtime/bin/linker64 /system/bin/tombstoned > $T/tombstoned.out 2>&1 &
  sleep 2
  echo "propd: $(cat $T/propd.pid)"
  for s in /servicemanager hwservicemanager allocator-service qspmhal-service tombstoned; do
    echo "${s#/}: $(pgrep -f "linker64 .*$s" | tr '\n' ' ')"
  done
}

probe() {
  # A probe killed while it held the request manager leaves /dev/video0 busy ("Operation already in progress").
  for x in $(pgrep -f "linker64 /vendor/bin/sleep"); do grep -q camx_probe /proc/$x/environ 2>/dev/null && kill -9 $x; done
  # A vendor program as the host (vendor/bin/sleep), so the vendor section of the linker config applies.
  cp $T/m2cam/libcamx_probe.so $C/vendor/etc/camera/asteroids/
  timeout ${1:-90} env LD_PRELOAD=/vendor/etc/camera/asteroids/libcamx_probe.so \
      chroot $C /apex/com.android.runtime/bin/linker64 /vendor/bin/sleep 600 > $T/probe.txt 2>&1
  echo rc=$?
  grep "^CAMX" $T/probe.txt
}

stacks() {
  # debuggerd -b: every thread's backtrace, through tombstoned's intercept socket.
  timeout 30 env LD_LIBRARY_PATH=$LP chroot $C /apex/com.android.runtime/bin/linker64 /system/bin/debuggerd -b $1
}

capture() {
  rm -f $T/data/capture.jpg $T/data/preview.ppm
  # The flash on as a torch unless CAMX_TORCH is set empty.
  CAMX_TORCH=${CAMX_TORCH-1} CAMX_CAPTURE=${1:-0} probe 120
  ls -la $T/data/capture.jpg $T/data/preview.ppm 2>/dev/null
}

server() {
  pkill -f "linker64 /vendor/bin/sleep 100000"
  mkdir -p $C/hmos-socket
  mountpoint -q $C/hmos-socket || mount --bind /hmos-root/dev/unix/socket $C/hmos-socket
  cp $T/m2cam/libcamserver.so $C/vendor/etc/camera/asteroids/
  setsid env LD_PRELOAD=/vendor/etc/camera/asteroids/libcamserver.so CAMSERVER_UID=${CAMSERVER_UID:-3028} \
      chroot $C /apex/com.android.runtime/bin/linker64 /vendor/bin/sleep 100000 > $T/camserver.txt 2>&1 < /dev/null &
  for i in $(seq 100); do grep -q "^CAMSERVER_READY" $T/camserver.txt && break; sleep 0.2; done
  grep "^CAMSERVER" $T/camserver.txt
}

stop() {
  for p in /proc/[0-9]*; do
    [ "$(readlink $p/root)" = $C ] && kill -9 ${p#/proc/} 2>/dev/null
  done
  pkill -9 -f "$T/logdw" 2>/dev/null
  rm -f $T/propd.pid
  sleep 1
  for m in $(awk -v c=$C '$2 == c || index($2, c "/") == 1 {print $2}' /proc/mounts | sort -r); do
    umount $m 2>/dev/null || umount -l $m
  done
  P=/sys/module/firmware_class/parameters/path
  grep -q "$T/fw," $P && echo -n "$(sed "s|$T/fw,||" $P)" > $P
  for r in /sys/class/remoteproc/*; do
    grep -q cdsp $r/name && [ "$(cat $r/state)" = running ] && echo stop > $r/state
  done
  echo "mounts left: $(grep -c " $C" /proc/mounts); processes left: $(for p in /proc/[0-9]*; do readlink $p/root; done 2>/dev/null | grep -c "^$C\$")"
}

"$@"
