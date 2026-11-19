#!/asteroids/bin/busybox sh
# Serial root shell on ttyGS0 that survives the handoff: it runs from the busybox in /asteroids/bin, not from M1's
# ramdisk, which HarmonyOS init removes. Replaces M1's own shell loop: pass that loop's PID and its shell's PID.
#     setsid /asteroids/bin/busybox sh /asteroids/boot/shell.sh $PPID $$ </dev/null >/dev/null 2>&1 &
export PATH=/asteroids/bin
sleep 2
kill $1 $2
while true; do
    sh -l </dev/ttyGS0 >/dev/ttyGS0 2>&1
    sleep 1
done
