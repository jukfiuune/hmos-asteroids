#!/bin/bash
# One look at whether the phone's pieces are up (from the laptop, phone on USB): scripts/health.sh
#   - the remote processors (ADSP, modem, Wi-Fi, CDSP: off unless a camera is open) and the Android-side helpers (lib.sh's respawn restarts them:
#     their exits since boot are counted from the boot log)
#   - HarmonyOS's services for audio, Bluetooth, sensors, vibration and the camera
#   - audio: the PAL server's failed writes and the VDI's lost links and recoveries
#   - native crashes since boot, and anything a test left running (the camera test root)
# Each line starts with ok or a "!" for something to look at.
cd "$(dirname "$0")/.."
python3 scripts/m0net.py sh '
export PATH=/asteroids/bin
T=/data/local/tmp
ok() { echo "ok  $*"; }
bad() { echo "!   $*"; }
check() { if [ -n "$2" ]; then ok "$1: $2"; else bad "$1: not running"; fi; }
echo "up $(cut -d. -f1 /proc/uptime) s, battery $(cat /sys/class/power_supply/battery/capacity)%"
for r in /sys/class/remoteproc/*; do
    n=$(sed "s/.*remoteproc-//" $r/name); s=$(cat $r/state)
    case $n:$s in cdsp:offline|*:running) ok "$n $s";; *) bad "$n $s";; esac
done
# Helpers in the Android root, by the library each preloads (or its command line).
pre() {
    for x in $(pgrep -f linker64); do
        tr "\0" "\n" < /proc/$x/environ 2>/dev/null | grep -q "^LD_PRELOAD=.*$1" && echo $x
    done | head -1
}
check "PAL server (audio)" "$(pre libsheng_pal_server)"
check "audioadsprpcd" "$(pgrep -f "audioadsprpcd audiopd" | head -1)"
check "Bluetooth bridge" "$(pre libsheng_bt_bridge)"
check "sscrpcd (sensors)" "$(pgrep -f "sscrpcd sensorspd" | head -1)"
check "RichTap (vibration)" "$(pre libasteroids_richtap)"
check "camserver (camera)" "$(pre libcamserver)"
for s in audio_server native_audio_hdi_publisher bluetooth_service native_bt_hdi_publisher sensors \
        native_vibrator_hdi_publisher camera_host; do
    check "$s" "$(pidof $s | cut -d" " -f1)"
done
r=$(grep -c "exited (" $T/asteroids-boot.log 2>/dev/null)
if [ "${r:-0}" = 0 ]; then ok "no helper restarts"; else
    bad "helper restarts: $(grep "exited (" $T/asteroids-boot.log | awk "{print \$2}" | sort | uniq -c | tr -s " \n" " ")"
    grep "exited (" $T/asteroids-boot.log | tail -3 | sed "s/^/      /"
fi
w=$(tail -200 $T/pal-server.txt 2>/dev/null | grep -c "op=3 rc=-")
[ "$w" = 0 ] && ok "audio writes going through" || bad "audio: $w of the PAL server last 200 lines are failed writes"
l=$(grep -c "SHENG_AUDIO_LINK_LOST\|SHENG_AUDIO_RECOVER" $T/sheng-audio-server.txt 2>/dev/null)
[ "${l:-0}" = 0 ] && ok "audio link steady" || { bad "audio link lost/recovered $l times:"; grep "LINK_LOST\|RECOVER" $T/sheng-audio-server.txt | tail -4 | sed "s/^/      /"; }
up=$(cut -d. -f1 /proc/uptime); now=$(date +%s)
c=$(for f in /data/log/faultlog/temp/cppcrash-* /data/log/faultlog/faultlogger/cppcrash-*; do
        [ -f "$f" ] || continue; t=$(stat -c %Y "$f"); [ $((now - t)) -lt $up ] && basename "$f"; done | sort -u)
[ -z "$c" ] && ok "no native crashes since boot" || bad "native crashes since boot, by process: $(for f in $c; do
    grep -h -o "\"PNAME\":\"[^\"]*\"" /data/log/faultlog/*/$f 2>/dev/null | head -1 | cut -d\" -f4; done | sort | uniq -c | tr -s " \n" " ")"
grep -q " /camroot " /proc/mounts && bad "the camera test root is up (sh /data/local/tmp/camtest/camx-test.sh stop)" || ok "no test roots left up"
pgrep -f powerlog-phone >/dev/null && echo "    (powerlog is running)"
' 40
