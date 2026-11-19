# The phone side of scripts/powerlog.sh: one line every 30 s of awake time, then any new kernel log lines that look
# like trouble as "!" lines. Run from M1's shell: setsid sh powerlog-phone.sh >> /data/local/tmp/powerlog.txt &
export PATH=/asteroids/bin
B=/sys/class/power_supply/battery
S=/sys/power/suspend_stats
# A helper is alive if some Android-root process has its preload library in its environment.
alive() {
    for x in $(pgrep -f linker64); do
        tr '\0' '\n' < /proc/$x/environ 2>/dev/null | grep -q "^LD_PRELOAD=.*$1" && { echo 1; return; }
    done
    echo 0
}
prev_busy=0; prev_total=0; seen=$(dmesg | tail -1 | cut -c1-15)
while :; do
    set -- $(head -1 /proc/stat)
    busy=$(($2 + $3 + $4 + $7 + $8)); total=$((busy + $5 + $6))
    cpu=$(( prev_total ? 100 * (busy - prev_busy) / (total - prev_total) : 0 )); prev_busy=$busy; prev_total=$total
    top=$(top -b -n1 -d1 | awk 'NR > 4 && $1 ~ /^[0-9]+$/ && $8 + 0 > 0 && $9 != "top" {print $9 ":" $8}' | head -3 | tr '\n' ' ')
    dsp=$(for r in /sys/class/remoteproc/*; do printf '%s=%s ' $(sed 's/.*remoteproc-//' $r/name) $(cat $r/state); done)
    echo "$(cut -d' ' -f1 /proc/uptime) $(date +%H:%M:%S) bat=$(cat $B/capacity)% i=$(($(cat $B/current_now) / 1000))mA" \
        "v=$(($(cat $B/voltage_now) / 1000))mV bl=$(cat /sys/class/backlight/panel0-backlight/actual_brightness)" \
        "susp=$(cat $S/success)/$(cat $S/fail) cpu=$cpu% top=[$top] pal=$(alive libsheng_pal_server)" \
        "bt=$(alive libsheng_bt_bridge) vib=$(alive /m2vib/) ssc=$(pgrep -f 'sscrpcd sensorspd' | wc -l)" \
        "arpc=$(pgrep -f audioadsprpcd | wc -l) $dsp"
    dmesg | awk -v s="$seen" 'f; index($0, s) == 1 {f = 1}' |
        grep -i -E 'ssr|subsys|crash|fatal|remoteproc|rproc|apr_|gpr|btfm|hci|bluetooth|audio|snd|q6|watchdog|oops|panic' |
        sed 's/^/! /' | cut -c1-200
    seen=$(dmesg | tail -1 | cut -c1-15)
    sleep 30
done
