#!/bin/bash
# Copies the haptics firmware (haptic_config.bin: the chip's RAM sine and layout; 100/101_Haptic.bin: RichTap's preset
# streams) out of Android's vendor image into $HARMONY_BUILD/fw/vibrator/, checked against firmware.sha256. Nothing's
# files, so they are not in the repo. Run on the build machine after Android's slot b images are in android-b/
# (README.md); assemble-boot.sh and build-asteroids.sh run it when the directory is missing.
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
H=$(cd "$(dirname "$0")" && pwd)
O=$HB/fw/vibrator
X=$HB/android-b/xv     # the extracted vendor image, kept for other steps
if [ ! -d $X/firmware ]; then
    rm -rf $X.new
    $HB/tools/erofs-utils/bin/fsck.erofs --extract=$X.new $HB/android-b/vendor_b.img >/dev/null
    chmod -R u+rwX $X.new && rm -rf $X && mv $X.new $X
fi
mkdir -p $O
for f in $(cut -d' ' -f3 $H/firmware.sha256); do install -m 644 $X/firmware/$f $O/$f; done
(cd $O && sha256sum -c --quiet $H/firmware.sha256)
echo "haptics firmware in $O"
