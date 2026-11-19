#!/bin/bash
# Stages Google Play for the Android container on the phone (laptop; HarmonyOS running, reached with ./port):
# build-anco-gapps.sh's trees from the build PC into /data/asteroids/anco/gapps, which prep.d/65-anco.sh binds over
# the guest's directories each boot. Labelled here: a file the guest image also has keeps the image's label (read
# from its mount at /anco_hmos), anything new is a guest system file. Remove the directory to go back.
#   stage-anco-gapps.sh
set -e
cd "$(dirname "$0")/../.."
. scripts/conf.sh
T=$(mktemp -d)
trap 'rm -rf $T' EXIT
pc_get $T fw/anco-gapps/anco-gapps.tar
./port put $T/anco-gapps.tar /data/local/tmp/anco-gapps.tar | tail -1
cat > $T/label.sh <<'EOF'
cd /data/asteroids/anco
find gapps | while read -r p; do
    ref=/anco_hmos/system/${p#gapps/}
    [ "$p" = gapps ] && ref=/anco_hmos/system
    l=$(ls -dZ "$ref" 2>/dev/null | cut -d' ' -f1)
    case "$l" in u:object_r:*) ;; *) l=u:object_r:aoco_system_file:s0 ;; esac
    chcon "$l" "$p"
done
EOF
./port put $T/label.sh /data/local/tmp/anco-gapps-label.sh | tail -1
./port sh 'set -e; D=/data/asteroids/anco; rm -rf $D/gapps; tar -xf /data/local/tmp/anco-gapps.tar -C $D;
    chroot /hmos-root /system/bin/sh /data/local/tmp/anco-gapps-label.sh;
    rm /data/local/tmp/anco-gapps.tar /data/local/tmp/anco-gapps-label.sh; sync; du -sh $D/gapps' 600
echo "staged; reboot to bind"
