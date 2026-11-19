#!/bin/sh
# Run on the phone (outside HarmonyOS), detached: a gesture repeated with the frame trace, GPU/CPU clocks and the
# composer's frame metrics, into /data/asteroids/gest/<gesture>/ (trace.txt, clocks.txt, metrics.txt). The app or screen it
# acts on must already be showing.
#   gesture-trace.sh cc       Control Center: swipe down from the top right, then back up, 6 times
#   gesture-trace.sh fling    flings up and down in the middle of the screen, 8 times each way
#   gesture-trace.sh shade    the notification shade (top left), 6 times
# Then gesture-summary.py on the copied directory. Trace points: the graphic and ace tags (trace_marker) plus sched_switch.
G=$1
O=/data/asteroids/gest/$G
T=/sys/kernel/debug/tracing
X=/asteroids/bin/tap
K=/sys/class/kgsl/kgsl-3d0
M=/hmos-root/data/local/tmp/sheng-composer-metrics.txt
P() { chroot /hmos-root /system/bin/param set "$@" >/dev/null; }
mkdir -p $O; rm -f $O/*
sh /asteroids/boot/wake.sh >/dev/null 2>&1
echo 0 > $T/tracing_on; echo > $T/trace
echo 32768 > $T/buffer_size_kb
echo 0 > $T/events/enable
echo 1 > $T/events/sched/sched_switch/enable
P debug.hitrace.tags.enableflags 824633720832   # graphic (bit 38) | ace (bit 39)
sleep 1
( while [ ! -f $O/stop ]; do
    echo "$(cut -d' ' -f1 /proc/uptime) $(cat $K/gpuclk) $(cat $K/gpu_busy_percentage | tr -d ' %') $(cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq | tr '\n' ' ')" >> $O/clocks.txt
    sleep 0.1
  done ) &
start=$(wc -l < $M)
echo 1 > $T/tracing_on
case $G in
cc)    for i in 1 2 3 4 5 6; do $X swipe 960 15 960 1300 200; sleep 1.5; $X swipe 540 2300 540 900 200; sleep 1.5; done ;;
shade) for i in 1 2 3 4 5 6; do $X swipe 150 15 150 1300 200; sleep 1.5; $X swipe 540 2300 540 900 200; sleep 1.5; done ;;
fling) for i in 1 2 3 4 5 6 7 8; do $X swipe 540 1900 540 600 120; sleep 1.2; $X swipe 540 600 540 1900 120; sleep 1.2; done ;;
esac
sleep 1
echo 0 > $T/tracing_on
touch $O/stop
P debug.hitrace.tags.enableflags 0
tail -n +$((start + 1)) $M > $O/metrics.txt
cat $T/trace > $O/trace.txt
echo 0 > $T/events/sched/sched_switch/enable
echo done > $O/done
