# The RichTap server (vibrator/richtap_server.c), HarmonyOS init service asteroids_richtap, in the Android root
# android-root.sh made: agmplay under Android's linker, its __libc_init taken over by libasteroids_richtap.so, with
# the device and effect configuration where nothing-rt_ics's init script puts them (RICHTAP_DEVICE_PATH, from
# vendor.vibrator.device, and ENV_RICHTAP_CONFIG_PATH); it listens on asteroids.richtap in /dev/unix/socket, for the
# vibrator VDI. The server is started again if it exits (lib.sh's respawn).
TAG=richtap
. /asteroids/boot/lib.sh
T=/data/local/tmp
[ -c $RICHTAP_DEVICE ] || fail "no $RICHTAP_DEVICE"
run() {
    [ -f $T/richtap.txt ] && mv $T/richtap.txt $T/richtap.old.txt  # else the wait below can see the last run's lines
    android LD_PRELOAD=/m2vib/libasteroids_richtap.so RICHTAP_DEVICE_PATH=$RICHTAP_DEVICE \
        ENV_RICHTAP_CONFIG_PATH=$RICHTAP_CONFIG -- /vendor/bin/agmplay -h > $T/richtap.txt 2>&1 </dev/null &
    p=$!
    until_ 100 sh -c "grep -q '^RICHTAP_READY=1' $T/richtap.txt || ! [ -d /proc/$p ]"
    grep -q '^RICHTAP_READY=1' $T/richtap.txt ||
        { log "server didn't start: $(tail -2 $T/richtap.txt | tr '\n' ' ')"; kill $p 2>/dev/null; return 1; }
    log "server ready (pid $p)"
    wait $p
}
respawn run
