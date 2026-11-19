#!/bin/bash
# Reports how the phone slept while it was unplugged (suspend can't happen on USB: the controller and the charger hold
# wakeup sources). Test: unplug, let the screen time out, wait a few minutes, wake it with the power key, plug back
# in, then run this. Shows the kernel's suspend counters, the suspend/resume lines and wakeup reasons from the kernel
# log, and the wakeup sources that woke or blocked it most.
cd "$(dirname "$0")/.."
python3 scripts/m0net.py sh '
cd /sys/power/suspend_stats
echo "suspends: $(cat success) ok, $(cat fail) failed (last failed device: $(cat last_failed_dev 2>/dev/null), errno $(cat last_failed_errno 2>/dev/null), step $(cat last_failed_step 2>/dev/null))"
echo "last hardware sleep: $(cat last_hw_sleep 2>/dev/null) us, total $(cat total_hw_sleep 2>/dev/null) us"
echo "--- kernel log"
dmesg | grep -E "PM: suspend (entry|exit)|PM: (Wakeup|resume|Some devices)|wakeup reason|Resume caused|Abort|Freezing|suspended for" | tail -40
echo "--- busiest wakeup sources (name, activations, wakeups, total ms, prevent-suspend ms)"
awk -F"\t+" "NR>1 {print \$1\",\", \$2, \$4, \$7, \$10}" /sys/kernel/debug/wakeup_sources | sort -k3 -n -r | head -12
echo "--- held right now"
awk -F"\t+" "NR>1 && \$6>0 {print \$1}" /sys/kernel/debug/wakeup_sources | tr "\n" " "; echo
echo "--- power HDI log"; tail -5 /data/local/tmp/asteroids-power-hdi.txt
' 30
