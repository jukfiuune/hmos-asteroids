# Things in /data and the root's RAM directories that HarmonyOS's services expect before they start.
D=/data/local
mkdir -p $D/tmp
# The boot animation's frames, where bootanimation_custom_config.json points.
cmp -s $A/etc/bootpic.zip $D/bootpic.zip || cp $A/etc/bootpic.zip $D/bootpic.zip
# Log files written by services that can't create files in /data/local/tmp themselves, with their owners: the boot
# animation and render_service, the battery and power HDI publishers (power_host), sheng's Wi-Fi category preload,
# the audio and sensor checks (shell), the bundle installer (system).
for f in bootanimation-adapter.log:0:0:666 render-stderr.log:0:0:666 render-mesa.log:0:0:666 \
         sheng-battery-hdi.txt:3025:3025:600 asteroids-power-hdi.txt:3025:3025:600 sheng-wifi-category-v134.txt:0:0:666 \
         audio-check.txt:2000:2000:644 audio-rec.txt:2000:2000:644 rec-mic.wav:2000:2000:644 rec-call.wav:2000:2000:644 \
         sensor-check.txt:2000:2000:644 asteroids-bundle.txt:1000:1000:644 asteroids-install.list:1000:1000:644; do
    IFS=: read n u g m <<E
$f
E
    [ -e $D/tmp/$n ] || : > $D/tmp/$n
    chown $u:$g $D/tmp/$n && chmod $m $D/tmp/$n
done
: > $D/tmp/bootanimation-adapter.log; : > $D/tmp/render-stderr.log; : > $D/tmp/render-mesa.log
# User storage without hmdfs: storage_daemon creates children of /storage/media with a plain mkdir.
mkdir -p $R/storage/media && chmod 0711 $R/storage/media
# sheng's database key store (libdbkeys.so) keeps each system UID's keys in its own 0700 directory.
K=/data/service/el1/public/database/.sheng-dbkeys
mkdir -p $K && chmod 0711 $K
while IFS=: read -r name pass uid gid rest; do
    case "$uid" in ""|*[!0-9]*) continue;; esac
    [ "$uid" -lt 10000 ] || continue
    mkdir -p $K/$uid && chown $uid:$gid $K/$uid && chmod 0700 $K/$uid
done < $R/system/etc/passwd
