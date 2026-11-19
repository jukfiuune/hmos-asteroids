#!/bin/sh
# Run on the phone: sh /data/asteroids/jank.sh (detached), then jank-summary.sh. Uses the composer's FRAME_GAPS metrics.
# Frame pacing during continuous flings in Settings' list: composer metrics written during the flings, and the GPU
# clock/busy and CPU policy frequencies every 100 ms. Output /data/asteroids/jank/.
T=/asteroids/bin/tap; O=/data/asteroids/jank; M=/hmos-root/data/local/tmp/sheng-composer-metrics.txt
mkdir -p $O; rm -f $O/*
sh /asteroids/boot/wake.sh >/dev/null 2>&1
$T swipe 540 2385 540 1500 120; sleep 2; $T 940 1760; sleep 4
G=/sys/class/kgsl/kgsl-3d0
( while [ ! -f $O/stop ]; do
    echo "$(cut -d' ' -f1 /proc/uptime) $(cat $G/gpuclk) $(cat $G/gpubusy | tr -s ' ' | tr ' ' /) $(cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq | tr '\n' ' ')" >> $O/clocks.txt
    sleep 0.1
  done ) &
start=$(wc -l < $M)
for i in 1 2 3 4 5 6 7 8; do
    $T swipe 540 1900 540 600 120; sleep 1.2
    $T swipe 540 600 540 1900 120; sleep 1.2
done
sleep 2
tail -n +$((start + 1)) $M > $O/metrics.txt
touch $O/stop; sleep 0.3
$T swipe 540 2385 540 1500 120
echo done > $O/done
