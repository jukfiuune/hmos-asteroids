# The ADSP's sensor framework (Android's vendor.sensors: sscrpcd sensorspd), HarmonyOS init service asteroids_sscrpcd;
# then asteroids.sensors.start=true, on which asteroids-desktop.cfg starts the sensor HDI. Needs the writable copy of
# persist/sensors android-root.sh binds. Restarted if it exits.
TAG=sensors
. /asteroids/boot/lib.sh
mountpoint -q /android/mnt/vendor/persist/sensors || fail "no persist copy; sensors not started"
# A fault in the DSP's sensor framework takes the whole ADSP down, and with recovery off (the default here) the kernel
# panics: let it restart the ADSP instead (audio then needs a reboot, but the phone stays up).
echo enabled > $(rproc $RPROC_ADSP)/recovery
run() {
    android -- /vendor/bin/sscrpcd sensorspd >> /data/local/tmp/sscrpcd.txt 2>&1 </dev/null &
    p=$!
    param set asteroids.sensors.start true >/dev/null
    log "sensors started"
    wait $p
}
: > /data/local/tmp/sscrpcd.txt
respawn run
