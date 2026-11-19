#!/bin/sh
# Run on the phone (outside HarmonyOS), detached: what the GPU and CPU do while apps are used by hand or by tap.
#   appload.sh start            starts sampling into /data/asteroids/appload/ (old data removed)
#   appload.sh mark <label>     marks the start of a segment ("qq-scroll", "idle"...)
#   appload.sh stop             stops; then appload-report.py on the laptop
# clocks.txt every 0.25 s: uptime, GPU clock, GPU busy %, CPU policy frequencies. procs.txt every 1 s: uptime, then
# pid, utime+stime (ticks), name for each process that has used any CPU. metrics.txt: the composer's frame metrics
# written meanwhile (FRAME_GAPS etc.).
O=/data/asteroids/appload
G=/sys/class/kgsl/kgsl-3d0
M=/hmos-root/data/local/tmp/sheng-composer-metrics.txt
case "$1" in
start)
    mkdir -p $O; rm -f $O/*
    wc -l < $M > $O/metrics.start 2>/dev/null
    ( while [ ! -f $O/stop ]; do
        echo "$(cut -d' ' -f1 /proc/uptime) $(cat $G/gpuclk) $(cat $G/gpu_busy_percentage | tr -d ' %') $(cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq | tr '\n' ' ')" >> $O/clocks.txt
        sleep 0.25
      done ) </dev/null >/dev/null 2>&1 &
    ( while [ ! -f $O/stop ]; do
        { echo "T $(cut -d' ' -f1 /proc/uptime)"
          for s in /proc/[0-9]*/stat; do read -r l < $s 2>/dev/null && echo "$l"; done |
              awk '{ c = $0; sub(/^.*\) /, "", c); split(c, f, " "); t = f[12] + f[13];
                     n = $0; sub(/^[0-9]+ \(/, "", n); sub(/\) .*$/, "", n); if (t > 0) print $1, t, n }'
        } >> $O/procs.txt
        sleep 1
      done ) </dev/null >/dev/null 2>&1 &
    echo started ;;
mark)
    echo "$(cut -d' ' -f1 /proc/uptime) $2" >> $O/marks.txt ;;
stop)
    touch $O/stop; sleep 1.2
    tail -n +$(( $(cat $O/metrics.start 2>/dev/null || echo 0) + 1 )) $M > $O/metrics.txt 2>/dev/null
    echo stopped ;;
esac
