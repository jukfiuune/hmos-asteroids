#!/bin/bash
# Unpacks the HarmonyOS donor (Huawei Pura 70 Pro+, HBN-AL10, 7.0.0.107) on the build machine into
# $HARMONY_BUILD/donor/pura70proplus-7.0.0.107/: the partition images (images/), the preload app package's preload.img
# (preload-pkg/x/), the extracted trees the builds read (x/, build/extract-donor.sh) and read-only mounts of system,
# vendor, sys_prod and chip_prod (mnt/; needs sudo). README.md step 2 says where the two zips come from.
#
#   unpack-donor.sh <update_full_base.zip> <update_full_preload_HBN-AL10_all_cn_R3.zip>
#   unpack-donor.sh --mount        (only the mounts, e.g. after a reboot of the build machine)
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
REPO=$(cd "$(dirname "$0")/.." && pwd)
D=$HB/donor/pura70proplus-7.0.0.107

mount_donor() {
    for p in system vendor sys_prod chip_prod; do
        mkdir -p $D/mnt/$p
        mountpoint -q $D/mnt/$p || sudo mount -t erofs -o loop,ro $D/images/$p.img $D/mnt/$p
    done
}
if [ "$1" = --mount ]; then mount_donor; exit; fi
[ $# -eq 2 ] || { sed -n 2,9p "$0"; exit 1; }
BASE=$(realpath "$1"); PRELOAD=$(realpath "$2")

echo "checking the zips (several minutes)"
echo "36fbb2d601ddf94a4b6542f5a58ecf5337344c0ad3cdd2090e4e894e771a154b  $BASE" | sha256sum -c --quiet
echo "$(head -1 $REPO/donor/pura70proplus-7.0.0.107/preload.sha256 | cut -d' ' -f1)  $PRELOAD" | sha256sum -c --quiet

# The base OTA's update.bin, split into partition images by unpack_huawei_package (SimomYung, pinned).
U=$HB/tools/unpack_huawei_package
if [ ! -d $U/.git ]; then
    git clone -q https://github.com/SimomYung/unpack_huawei_package $U
    git -C $U -c advice.detachedHead=false checkout -q 928e61839a521218bcc13a2c4e62661bc212bc8d
fi
if [ ! -f $D/images/system.img ]; then
    rm -rf $D/images.new && mkdir -p $D/images.new
    unzip -p "$BASE" update.bin > $D/images.new/update.bin
    (cd $D/images.new && python3 -I $U/unpack_huawei_package.py && rm update.bin)
    mv $D/images.new $D/images
fi
# The preload package: its own update.bin holds preload.img.
if [ ! -f $D/preload-pkg/x/preload.img ]; then
    rm -rf $D/preload-pkg/x.new && mkdir -p $D/preload-pkg/x.new
    unzip -p "$PRELOAD" update.bin > $D/preload-pkg/x.new/update.bin
    (cd $D/preload-pkg/x.new && python3 -I $U/unpack_huawei_package.py && rm update.bin)
    mv $D/preload-pkg/x.new $D/preload-pkg/x
fi
(cd $D/images && sha256sum -c --quiet $REPO/donor/pura70proplus-7.0.0.107/images.sha256)
(cd $D/preload-pkg/x && tail -1 $REPO/donor/pura70proplus-7.0.0.107/preload.sha256 | sha256sum -c --quiet)
bash $REPO/build/extract-donor.sh
mount_donor
echo "donor in $D"
