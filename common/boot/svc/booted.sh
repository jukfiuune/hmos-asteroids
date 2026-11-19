# Once the desktop is up (HarmonyOS init service asteroids_booted, once, on bootevent.boot.completed=true):
#   - Android's CPU and memory-bus tuning (cpu-tuning.sh): WALT instead of the command line's performance governor.
#   - Wake-on-WLAN triggers on the Wi-Fi device (wifi/wowlan.c): without them every suspend disconnects Wi-Fi, and
#     the driver's teardown aborts the suspend.
#   - Direct scanout back on (route.sh turned it off for the boot animation).
#   - msm_video's decoder and encoder nodes opened up for av_codec_service (media): HarmonyOS's ueventd gives every
#     /dev/video* to camera_host, 0660.
#   - Apps' view of the user's files (/storage/Users/currentUser in their sandboxes, from
#     /mnt/user/100/sharefs/docs/currentUser): Huawei's storage_daemon mounts sharefs there, which this kernel lacks,
#     leaving an empty directory (Files showed only the gallery). Once the user is unlocked it gets a bind of the real
#     Docs. sharefs would also have limited each app to its own share; a bind shows all of Docs.
#   - The panel on: the display wake asteroids-desktop.cfg sends with boot-completed can reach powermgr before
#     powermgr has acted on boot-completed, which drops it, leaving the panel off. Woken again until the panel is on
#     (the power-shell service, which has powermgr's permission; a root shell hasn't).
#   - The panel capture loop (capture.sh), unless /data/asteroids/no-capture exists.
#   - A minute later, with SceneBoard still the same process, the running slot marked successful (slot_success), so
#     the bootloader stops spending its retries. HarmonyOS init panics and reboots when SceneBoard keeps crashing, so a
#     slot that gets this far boots.
TAG=after-boot
. /asteroids/boot/lib.sh
log "desktop up"
sh /asteroids/boot/svc/cpu-tuning.sh
(
    until_ 1200 [ -d /sys/class/ieee80211/phy0 ] && log "$(/asteroids/bin/wowlan 0)"
) &
rm -f /dev/sheng-display-route-mode
chmod 666 $VIDEO_NODES
(
    # storage_daemon makes its (empty) mount there when the user is unlocked; the bind goes over it.
    U=/mnt/user/100/sharefs/docs/currentUser
    DOCS=/data/service/el2/100/hmdfs/account/files/Docs
    until grep -q " $U " /proc/1/mountinfo; do sleep 2; done
    if grep -q " $DOCS $U " /proc/1/mountinfo; then log "user docs already mounted"
    elif mount --bind $DOCS $U; then log "user docs mounted for apps"
    else log "user docs mount failed"; fi
) &
on() { grep -A1 '^plane\[' /sys/kernel/debug/dri/0/state | grep -q 'crtc=crtc'; }
for i in 1 2 3 4 5 6 7 8 9 10; do
    on && break
    /system/bin/begetctl start_service asteroids_display_wake >/dev/null 2>&1
    sleep 3
done
on && log "display on" || log "display still off"
[ -e /data/asteroids/no-capture ] || /system/bin/begetctl start_service asteroids_capture >/dev/null 2>&1
scene=$(pidof com.ohos.sceneboard)
sleep 60
if [ -n "$scene" ] && [ "$(pidof com.ohos.sceneboard)" = "$scene" ]; then
    /asteroids/bin/slot_success mark 2>&1 | while read -r l; do log "$l"; done
else
    log "SceneBoard restarted (was '$scene'); slot not marked"
fi
wait
