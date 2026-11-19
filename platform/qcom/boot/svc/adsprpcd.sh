# The ADSP's FastRPC daemon for its audio domain (Android's audioadsprpcd), which loads the DSP's dynamic modules from
# vendor/dsp: AGM's graph init fails without it. HarmonyOS init service asteroids_adsprpcd; restarted if it exits.
TAG=adsprpcd
. /asteroids/boot/lib.sh
run() { android -- /vendor/bin/audioadsprpcd audiopd adsp >> /data/local/tmp/audioadsprpcd.txt 2>&1 </dev/null; }
: > /data/local/tmp/audioadsprpcd.txt
respawn run
