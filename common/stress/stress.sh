#!/bin/sh
# asteroids: UI stress test, run detached on the phone from M1's shell (busybox):
#   setsid sh /asteroids/stress/stress.sh <minutes> </dev/null >/dev/null 2>&1 &
# Loops: open each home-screen app and go home, and in Gallery open the newest picture
# and turn it through all four rotations (asteroids.motion.force_rotation, read by our motion VDI). A sampler
# records every 5 s: the pids of the services that must not restart, fd counts and RSS of the graphics stack,
# MemAvailable, GPU busy/clock and temperatures. Output: /data/asteroids/stress/{events,samples}.txt; stress-report.py
# on the laptop summarises them. Taps are at the home screen's positions (1080x2392 panel). The browser reopens on
# its last page (load one first): it is scrolled, a result opened and left again, and the page turned.
# power-shell's timeout override is refused from a root shell, so taps on the status bar keep the screen on.
MIN=${1:-10}
O=/data/asteroids/stress
R=/hmos-root
T=/asteroids/bin/tap
mkdir -p $O && rm -f $O/*.txt
param() { chroot $R /system/bin/param set "$@" >/dev/null 2>&1; }
log() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $O/events.txt; }
home() { $T swipe 540 2385 540 1500 120; sleep 2; }
awake() { $T 540 40; }  # status bar: resets powermgr's screen timer
SERVICES="render_service composer_host allocator_host foundation com.ohos.sceneboard msdp native_motion_hdi_publisher sensors audio_server multimodalinput"
GPU=/sys/class/kgsl/kgsl-3d0

sample() {
    while [ -f $O/running ]; do
        line="t=$(cut -d' ' -f1 /proc/uptime) memavail_kb=$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
        line="$line gpubusy=$(cat $GPU/gpubusy 2>/dev/null | tr ' ' '/') gpuclk=$(cat $GPU/gpuclk 2>/dev/null)"
        line="$line gputemp=$(cat $GPU/temp 2>/dev/null) bl=$(cat /sys/class/backlight/panel0-backlight/bl_power)"
        for s in $SERVICES; do
            p=$(pidof $s | cut -d' ' -f1)
            fds=$(ls /proc/$p/fd 2>/dev/null | wc -l)
            rss=$(grep VmRSS /proc/$p/status 2>/dev/null | tr -s ' ' | cut -d' ' -f2)
            line="$line $s=${p:-none}/${fds}fd/${rss:-0}kB"
        done
        echo "$line" >> $O/samples.txt
        sleep 5
    done
}

touch $O/running
(timeout $(( MIN * 60 + 120 )) chroot $R /system/bin/hilog > $O/hilog.txt 2>&1 &)
chroot $R /system/bin/power-shell timeout -o $(( (MIN + 2) * 60000 )) >/dev/null 2>&1
sh /asteroids/boot/wake.sh >/dev/null 2>&1
[ "$(cat /sys/class/backlight/panel0-backlight/bl_power)" = 0 ] || log "panel stayed off: the run is blind"
$T swipe 540 2100 540 700 250; sleep 2
sample &
END=$(( $(cut -d. -f1 /proc/uptime) + MIN * 60 ))
cycle=0
while [ $(cut -d. -f1 /proc/uptime) -lt $END ]; do
    cycle=$((cycle + 1))
    log "cycle $cycle"
    # Not Settings: a blind tap there landed on its Wi-Fi switch (stress 2 turned Wi-Fi off).
    for app in "gallery 670 1760" "appgallery 140 1760" "phone 140 2200" "messages 405 2200" "browser 675 2200"; do
        set -- $app
        home
        log "open $1"
        $T $2 $3; sleep 4; awake
        if [ $1 = gallery ]; then
            $T 890 1170; sleep 3  # the newest picture
            log "gallery photo"
            for r in 1 0 3 0 2 0; do
                param asteroids.motion.force_rotation $r; log "rotation $r"; sleep 3; awake
            done
            param asteroids.motion.force_rotation ""
            $T 60 260; sleep 2  # back to the grid
        fi
        if [ $1 = browser ]; then
            sleep 4
            for i in 1 2 3 4 5 6; do $T swipe 540 1800 540 700 200; sleep 1; done
            for i in 1 2 3; do $T swipe 540 700 540 1800 200; sleep 1; done
            log "browser scrolled"
            $T 540 1400; sleep 8; log "browser link"; awake
            $T 108 2241; sleep 4; log "browser back"
            param asteroids.motion.force_rotation 1; log "rotation 1"; sleep 4; awake
            $T swipe 800 1200 300 1200 200; sleep 1  # rotated: a scroll across the panel
            param asteroids.motion.force_rotation 0; log "rotation 0"; sleep 3
            param asteroids.motion.force_rotation ""
        fi
        [ $(cut -d. -f1 /proc/uptime) -lt $END ] || break
    done
done
home
param asteroids.motion.force_rotation ""
chroot $R /system/bin/power-shell timeout -r >/dev/null 2>&1
rm -f $O/running
dmesg | grep -E "exit with (signal|code : [1-9])|ServiceReap|bootfail|XCollie" > $O/exits.txt
log "done"
