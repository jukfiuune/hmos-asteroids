#!/asteroids/bin/busybox sh
# Blackbox for the M1 handoff: every 2 seconds, writes the kernel log, a process list, the root directory, the mount
# table and the USB gadget state to /data/asteroids/blackbox and syncs, so they survive a hard reset (which clears pstore).
# Runs outside the HarmonyOS chroot, with its own busybox in /asteroids/bin: HarmonyOS init removes M1's ramdisk
# (and with it /bin) right after the handoff. The previous run's files move to *.prev. Start it from the serial shell
# before touching /tmp/m1-go:
#     setsid /asteroids/boot/blackbox.sh </dev/null >/dev/null 2>&1 &
export PATH=/asteroids/bin
L=/data/asteroids/blackbox
mkdir -p $L
for f in dmesg ps usb beat root mounts; do [ -e $L/$f.txt ] && mv $L/$f.txt $L/$f.prev.txt; done
n=0
while true; do
    n=$((n + 1))
    dmesg > $L/dmesg.tmp && mv $L/dmesg.tmp $L/dmesg.txt
    ps -o pid,ppid,user,stat,args > $L/ps.txt 2>&1
    ls -la / > $L/root.txt 2>&1
    cat /proc/self/mountinfo > $L/mounts.txt 2>&1
    { ls /sys/class/udc; for g in /sys/kernel/config/usb_gadget/*; do
        echo "$g UDC=$(cat $g/UDC) functions=$(ls $g/functions | tr '\n' ' ')"; done; } > $L/usb.txt 2>&1
    echo "beat $n uptime $(cut -d' ' -f1 /proc/uptime 2>&1) go=$([ -e /tmp/m1-go ] && echo yes)" > $L/beat.txt
    sync
    sleep 2
done
