# The Android root the audio, sensor, Bluetooth and vibration helpers run in (HarmonyOS init service asteroids_android,
# once, at post-fs-data; init/asteroids-helpers.cfg). /android is the android partition in super (android/
# build-android-helpers.sh: the files these helpers use out of Android's slot b system, vendor and odm, and the runtime
# and VNDK 34 APEXes), with:
#   - /dev, /proc, /sys; /data from /data/asteroids/android
#   - our libraries the helpers preload: /m2 (audio), /m2bt (Bluetooth), /m2vib (vibration); HarmonyOS's
#     /dev/unix/socket at /hmos-socket, where the bridges listen
#   - the DSP's modules partition on vendor/dsp, persist (read-only) where the HALs look, Bluetooth's firmware partition
#     at /bt_firmware
#   - a writable copy of persist/sensors over it: on the read-only persist the DSP's registry write failed and took the
#     ADSP down (and with it the phone)
#   - vendor/etc with a RAM overlay holding the files Android's audio HAL would have bound into place from this sound
#     card's directory (PAL looks for them in vendor/etc without the SKU property), PAL's context manager off (it needs
#     hwbinder; pal_no_hwbinder.c covers the other use) and the speaker backend's format for AGM (backend_conf.xml)
# Then asteroids.android.ready=true, on which init starts the helpers.
TAG=android
. /asteroids/boot/lib.sh
A=/android
I=/data/asteroids/android
grep -q " $A erofs " /proc/mounts || mount -t erofs -o ro /dev/mapper/android$SLOT $A || fail "android$SLOT not mounted"
b() { mountpoint -q $2 || mount --bind $1 $2 || fail "bind $1 on $2"; }
b /dev $A/dev
b /sys $A/sys
mountpoint -q $A/proc || mount -t proc proc $A/proc
mkdir -p $I/vendor/audio $I/vendor/bluetooth
b $I $A/data
b /asteroids/android/audio $A/m2
b /asteroids/android/bluetooth $A/m2bt
b /asteroids/android/vibrator $A/m2vib
b /dev/unix/socket $A/hmos-socket
mountpoint -q $A/vendor/dsp || mount -t ext4 -o ro,norecovery /dev/$(part $DSP_PART$SLOT) $A/vendor/dsp ||
    fail "$DSP_PART$SLOT not mounted"
mountpoint -q $A/bt_firmware ||
    mount -t vfat -o ro,shortname=lower /dev/$(part $BT_FIRMWARE_PART$SLOT) $A/bt_firmware || log "$BT_FIRMWARE_PART$SLOT not mounted"
P=$A/mnt/vendor/persist
mountpoint -q $P || mount -t ext4 -o ro,noload /dev/$(part persist) $P || log "persist not mounted"
S=$I/vendor/sensors-persist
if [ ! -d $S ] && mountpoint -q $P; then
    mkdir -p $S.new && cp -a $P/sensors/. $S.new/ && chown -R 1000:1000 $S.new && mv $S.new $S
fi
[ -d $S/registry ] && b $S $P/sensors
if ! mountpoint -q $A/vendor/etc; then
    E=$A/etc-rw
    mount -t tmpfs -o size=1m tmpfs $E && mkdir -p $E/upper $E/work || fail "etc tmpfs"
    cp $A/vendor/etc/$AUDIO_MIXER $E/upper/
    sed 's/key="context_manager_enable" value ="true"/key="context_manager_enable" value ="false"/' \
        $A/vendor/etc/$AUDIO_RESOURCES > $E/upper/$(basename $AUDIO_RESOURCES)
    cp /asteroids/android/audio/backend_conf.xml $E/upper/
    mount -t overlay overlay -o lowerdir=$A/vendor/etc,upperdir=$E/upper,workdir=$E/work $A/vendor/etc ||
        fail "vendor/etc overlay"
fi
log "android root ready"
param set asteroids.android.ready true >/dev/null
