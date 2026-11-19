# Shared by the asteroids partition's scripts (sourced, with TAG set to the log tag first). They run either in M1's
# root before the handoff (boot/prep.sh and its steps, HarmonyOS's root at /hmos-root) or as HarmonyOS init services
# (boot/svc/*.sh, in HarmonyOS's root, where /asteroids is bound in). Either way with this busybox.
export PATH=/asteroids/bin
. /asteroids/boot/device.conf
# Log lines go to the kernel log and to /data/local/tmp/asteroids-boot.log (prep.sh starts it each boot): the kernel
# log wraps within the first minute.
log() { echo "hmos-${TAG:-asteroids}: $*" > /dev/kmsg; echo "$(cut -d' ' -f1 /proc/uptime) $TAG: $*" >> /data/local/tmp/asteroids-boot.log; }
fail() { log "$*"; exit 1; }
# HarmonyOS's root as seen from here.
if [ -d /hmos-root/system ]; then HR=/hmos-root; else HR=; fi
param() { if [ -n "$HR" ]; then chroot $HR /system/bin/param "$@"; else /system/bin/param "$@"; fi; }
begetctl() { if [ -n "$HR" ]; then chroot $HR /system/bin/begetctl "$@"; else /system/bin/begetctl "$@"; fi; }
# The running slot's suffix (_a or _b).
SLOT=$(sed -n 's/^androidboot.slot_suffix = "\(_[ab]\)"$/\1/p' /proc/bootconfig)
# part <name>: the block device (sdXN) of a partition, by its GPT name.
part() {
    for u in /sys/class/block/*/uevent; do
        grep -q "^PARTNAME=$1\$" $u && { basename $(dirname $u); return; }
    done
}
# rproc <name>: a remote processor's sysfs directory.
rproc() {
    for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name)" = "$1" ] && echo $r; done
}
# until_ <tenths of a second> <command...>: runs the command until it succeeds or the time is up.
until_() {
    n=$1; shift
    i=0
    until "$@"; do [ $i -ge $n ] && return 1; sleep 0.1; i=$((i + 1)); done
}
# respawn <command...>: for the boot/svc scripts that start a helper and wait on it. Runs the command (a shell function
# that starts the helper, returns non-zero if it doesn't come up, and otherwise waits for it) again each time it
# returns, so a helper that dies comes back without a reboot. Each exit is logged; the pause before the next start
# doubles from 2 s up to 60 s and goes back to 2 s after a run of over 10 minutes.
respawn() {
    d=2
    while :; do
        t0=$(cut -d. -f1 /proc/uptime)
        "$@"; rc=$?
        t=$(($(cut -d. -f1 /proc/uptime) - t0))
        [ $t -gt 600 ] && d=2
        log "exited ($rc) after ${t}s; starting again in ${d}s"
        sleep $d
        d=$((d * 2)); [ $d -gt 60 ] && d=60
    done
}
# android [VAR=value...] -- <binary> [args...]: runs an Android binary in the Android root (/android,
# svc/android-root.sh) under Android's linker, with Android's library path and the variables given (no spaces in them).
android() {
    e=
    while [ "$1" != -- ]; do e="$e $1"; shift; done
    shift
    env LD_LIBRARY_PATH=$ANDROID_LIBPATH $e chroot /android /apex/com.android.runtime/bin/linker64 "$@"
}
