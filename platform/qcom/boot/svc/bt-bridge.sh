# sheng's Bluetooth path (HarmonyOS init service asteroids_bt_bridge, in the Android root android-root.sh made):
#   1. The chip's address: Android's vendor init sets it from the modem, and the HAL otherwise makes up a new one each
#      run. One is generated once (22:22:..., the HAL's own pattern), kept in
#      /data/asteroids/android/vendor/bluetooth/bdaddr, handed to the bridge and written for HarmonyOS's stack.
#   2. sheng's bridge: agmplay under Android's linker, its __libc_init taken over by libsheng_bt_bridge.so, listening
#      on sheng.bt.h4 in /dev/unix/socket; it powers the chip (btpower, the UART) only when the HCI service connects.
#   3. asteroids.bluetooth.start=true, on which asteroids-desktop.cfg starts the HCI HDI publisher.
# Logs to the kernel log as hmos-bluetooth and to /data/local/tmp/start-bluetooth.log. The bridge is started again if
# it exits (lib.sh's respawn).
TAG=bluetooth
. /asteroids/boot/lib.sh
T=/data/local/tmp
: > $T/start-bluetooth.log
log() { echo "hmos-bluetooth: $*" > /dev/kmsg; echo "$*" >> $T/start-bluetooth.log
      echo "$(cut -d' ' -f1 /proc/uptime) bluetooth: $*" >> $T/asteroids-boot.log; }
[ -c $BT_POWER ] && [ -c $BT_UART ] || fail "no $BT_POWER or $BT_UART"

F=/data/asteroids/android/vendor/bluetooth/bdaddr
if [ ! -s $F ]; then
    printf '22:22:%s\n' "$(od -An -tx1 -N4 /dev/urandom | tr -d ' \n' | sed 's/../&:/g; s/:$//')" > $F.new && mv $F.new $F
fi
bdaddr=$(cat $F)
mkdir -p /data/vendor/bluetooth /data/log/bt /data/service/el1/public/bluetooth
chown 3021:3021 /data/vendor/bluetooth && chmod 0770 /data/vendor/bluetooth
chown 1002:1002 /data/log/bt /data/service/el1/public/bluetooth && chmod 0711 /data/service/el1/public/bluetooth
echo "$bdaddr" | tr a-f A-F > /data/vendor/bluetooth/btmac.txt
chown 3021:3021 /data/vendor/bluetooth/btmac.txt && chmod 0640 /data/vendor/bluetooth/btmac.txt

run() {
    rm -f /dev/unix/socket/sheng.bt.h4
    [ -f $T/bt-bridge.txt ] && mv $T/bt-bridge.txt $T/bt-bridge.old.txt  # else the wait below sees the last run's lines
    android LD_PRELOAD=/m2bt/libsheng_bt_bridge.so SHENG_BT_BDADDR=$bdaddr \
        SHENG_BT_ORIGINAL_LIBRARY=/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so \
        -- /vendor/bin/agmplay -h > $T/bt-bridge.txt 2>&1 </dev/null &
    p=$!
    until_ 100 sh -c "grep -q '^BT_BRIDGE_LISTENING=1' $T/bt-bridge.txt || ! [ -d /proc/$p ]"
    grep -q '^BT_BRIDGE_LISTENING=1' $T/bt-bridge.txt ||
        { log "bridge didn't start: $(tail -2 $T/bt-bridge.txt | tr '\n' ' ')"; kill $p 2>/dev/null; return 1; }
    log "bridge listening (pid $p, address $bdaddr)"
    if [ -z "$started" ]; then
        B=$T/sheng-bluetooth-hdi.txt
        : > $B; chown 3021:3021 $B; chmod 600 $B
        param set asteroids.bluetooth.start true >/dev/null
        log "HCI service started"
        started=1
    else
        # HarmonyOS's Bluetooth stack doesn't notice the bridge going: it keeps the dead HCI channel (tested). With
        # its HDI and service restarted it powers the chip on again through the new bridge, if Bluetooth was on.
        for s in bluetooth_service asteroids_bluetooth_hdi; do begetctl stop_service $s >/dev/null 2>&1; done
        sleep 1; begetctl start_service asteroids_bluetooth_hdi >/dev/null 2>&1
        sleep 1; begetctl start_service bluetooth_service >/dev/null 2>&1
        log "HCI service and bluetooth_service restarted"
    fi
    wait $p
}
started=
respawn run
