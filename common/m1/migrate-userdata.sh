#!/bin/sh
# One-time move from the old userdata layout ("hmosdata", before 2026-11-15) to the current one, where userdata is
# HarmonyOS's /data. M1 init runs it, before anything uses userdata, when it finds the old layout (data/service/el1
# and m1/ at the top). Everything is a rename within the filesystem:
#   data/*            -> /                       HarmonyOS's data
#   android/data      -> asteroids/android       the Bluetooth address and the sensor registry copy
#   modem/readwrite   -> asteroids/modem         the modem's read-write files (tqftpserv)
#   m1/clock-offset   -> asteroids/clock-offset
# Everything else at the old top (the image files, Android's images, the old bundle, backups, logs) ends up in
# asteroids/old-hmosdata, to be deleted by hand once the new layout boots.
D=/data
O=$D/.old-hmosdata
H=$D/.hmos
set -e
mkdir $O
for f in $D/* $D/.[!.]*; do
    case "$f" in $D/data|$D/lost+found|$O|"$D/.[!.]*") continue;; esac
    [ -e "$f" ] && mv "$f" $O/
done
mv $D/data $H
for f in $H/* $H/.[!.]*; do
    [ -e "$f" ] && mv "$f" $D/
done
rmdir $H
mkdir -p $D/asteroids
[ -d $O/android/data ] && mv $O/android/data $D/asteroids/android
[ -d $O/modem/readwrite ] && mv $O/modem/readwrite $D/asteroids/modem
[ -f $O/m1/clock-offset ] && mv $O/m1/clock-offset $D/asteroids/clock-offset
mv $O $D/asteroids/old-hmosdata
echo "userdata moved to the new layout; the old files are in /data/asteroids/old-hmosdata"
