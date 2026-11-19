#!/bin/sh
# Raw ftrace of the fling test (HarmonyOS's hitrace refuses this kernel): the graphic and ace tags' trace points
# (trace_marker) plus sched_switch, into /data/asteroids/jank/trace.txt.
T=/sys/kernel/debug/tracing
P() { chroot /hmos-root /system/bin/param set "$@" >/dev/null; }
echo 0 > $T/tracing_on; echo > $T/trace
echo 32768 > $T/buffer_size_kb
echo 0 > $T/events/enable
echo 1 > $T/events/sched/sched_switch/enable
P debug.hitrace.tags.enableflags 824633720832   # graphic (bit 38) | ace (bit 39)
sleep 1
echo 1 > $T/tracing_on
sh /data/asteroids/jank.sh   # common/stress/jank.sh
echo 0 > $T/tracing_on
P debug.hitrace.tags.enableflags 0
cat $T/trace > /data/asteroids/jank/trace.txt
echo 0 > $T/events/sched/sched_switch/enable
echo done > /data/asteroids/jank/tdone
