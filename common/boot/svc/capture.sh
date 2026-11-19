# The panel capture loop (HarmonyOS init service asteroids_capture, which booted.sh starts unless
# /data/asteroids/no-capture exists): capture_scanout once a second into /data/local/tmp/cap/fNN.ppm, a ring of 60,
# each with a .time file, which scripts/screens.sh (port screens) pulls.
export PATH=/asteroids/bin
C=/data/local/tmp/cap
mkdir -p $C
i=0
while true; do
    f=$C/f$(printf %02d $((i % 60))).ppm
    rm -f $f
    /system/bin/capture_scanout $f 4 >/dev/null 2>&1
    date +%T > $f.time
    i=$((i + 1))
    sleep 1
done
