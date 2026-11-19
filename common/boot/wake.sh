#!/asteroids/bin/busybox sh
# On the phone (outside HarmonyOS): turns the display on and unlocks if it is off, so a test can start right away.
# Power key, then a swipe up on the lock screen (no credential is set). Leaves an awake display alone.
T=/asteroids/bin/tap
# The panel can be dark with its plane still attached, so the backlight counts too.
on() {
    grep -A1 '^plane\[' /sys/kernel/debug/dri/0/state | grep -q 'crtc=crtc' &&
        [ "$(cat /sys/class/backlight/panel0-backlight/bl_power)" = 0 ]
}
on && exit 0
# A press can go unanswered (the first one after boot often is): check for up to 3 s, then press again.
for try in 1 2 3; do
    $T key 116 2>/dev/null
    for i in 1 2 3 4 5 6; do sleep 0.5; on && break 2; done
done
sleep 1
$T swipe 540 2100 540 700 2>/dev/null
sleep 1.5
