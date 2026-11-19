#!/bin/bash
# Logs how the phone does while unplugged, for battery drain and for what breaks off USB (the phone side is
# scripts/powerlog-phone.sh). From the laptop:
#   scripts/powerlog.sh start     starts the logger on the phone (it keeps going unplugged, until stop or a reboot)
#   scripts/powerlog.sh report    after replugging: a summary, and the whole log in out/powerlog.txt
#   scripts/powerlog.sh stop
# Every 30 s of awake time one line: uptime, clock, battery %, current (mA, negative = discharging), voltage,
# backlight (actual_brightness: 0 with the screen off), suspends ok/failed, CPU busy % and the busiest processes, which helpers are alive and the DSP states;
# new kernel log lines that look like trouble follow as "!" lines.
cd "$(dirname "$0")/.."
LOG=/data/local/tmp/powerlog.txt
case "$1" in
start)
    python3 scripts/m0net.py sh "pkill -f powerlog-phone; true" 10
    python3 scripts/m0net.py put scripts/powerlog-phone.sh /data/local/tmp/powerlog-phone.sh >/dev/null
    python3 scripts/m0net.py sh "setsid sh /data/local/tmp/powerlog-phone.sh >> $LOG 2>&1 < /dev/null &
        sleep 4; tail -1 $LOG" 20
    ;;
stop)
    python3 scripts/m0net.py sh "pkill -f powerlog-phone; echo stopped; wc -l $LOG" 15
    ;;
report)
    mkdir -p out
    python3 scripts/m0net.py get $LOG out/powerlog.txt >/dev/null
    echo "out/powerlog.txt: $(wc -l < out/powerlog.txt) lines"
    # Average current with the screen on (backlight > 0) and off, and each change in helpers or DSPs.
    awk '$1 !~ /^!/ {
        ma = 0; bl = 0; h = ""
        for (k = 1; k <= NF; k++) {
            split($k, kv, "=")
            if (kv[1] == "i") ma = kv[2] + 0
            if (kv[1] == "bl") bl = kv[2] + 0
            if (kv[1] ~ /^(pal|bt|vib|ssc|arpc|adsp|mss|wpss|cdsp)$/) h = h " " $k
        }
        st = bl > 0 ? "on" : "off"; n[st]++; sum[st] += ma
        if (h != last) { print $2, "helpers/DSPs:" h; last = h }
    }
    END { for (s in n) printf "screen %s: %d samples, average %d mA\n", s, n[s], sum[s] / n[s] }' out/powerlog.txt
    grep "^!" out/powerlog.txt | head -40
    ;;
*)
    sed -n 2,9p "$0"
    ;;
esac
