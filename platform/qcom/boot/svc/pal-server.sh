# sheng's PAL server (HarmonyOS init service asteroids_pal): agmplay under Android's linker, its __libc_init taken over
# by libsheng_pal_server.so, which starts AGM and PAL and serves @sheng.pal.pcm.v1 to the HarmonyOS audio VDI. Once it
# is ready, asteroids.audio.start=true, on which asteroids-desktop.cfg starts audio_host and audio_server. If the server
# exits it is started again (lib.sh's respawn); the VDI reconnects on its next write.
TAG=audio
. /asteroids/boot/lib.sh
T=/data/local/tmp
until_ 600 sh -c '! grep -q "no soundcards" /proc/asound/cards' || fail "no sound card"
for f in sheng-audio-server.txt:1041:1000 sheng-audio-host.txt:3127:3127; do
    n=${f%%:*}; o=${f#*:}
    : > $T/$n; chown ${o%:*}:${o#*:} $T/$n; chmod 600 $T/$n
done
run() {
    # AGM's graph init fails (-19) until audioadsprpcd has the DSP's audio domain open, which takes a moment: retry.
    n=0
    while :; do
        n=$((n + 1))
        [ -f $T/pal-server.txt ] && mv $T/pal-server.txt $T/pal-server.old.txt  # else the wait below sees old lines
        android LD_PRELOAD=/m2/libpal_no_hwbinder.so:/m2/libsheng_pal_lifecycle.so:/m2/libsheng_pal_server.so:/vendor/lib64/libagm.so \
            -- /vendor/bin/agmplay -h > $T/pal-server.txt 2>&1 </dev/null &
        p=$!
        until_ 150 sh -c "grep -q '^PAL_SERVER_READY=1' $T/pal-server.txt || ! [ -d /proc/$p ]"
        grep -q '^PAL_SERVER_READY=1' $T/pal-server.txt && break
        kill $p 2>/dev/null
        [ $n -lt 10 ] || { log "PAL server didn't start: $(grep PAL_SERVER $T/pal-server.txt | tr '\n' ' ')"; return 1; }
        sleep 2
    done
    log "PAL server ready (pid $p)"
    param set asteroids.audio.start true >/dev/null
    wait $p
}
respawn run
