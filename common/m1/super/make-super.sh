#!/bin/bash
# Lays out super for HarmonyOS (run on the laptop; reads the image sizes from the build PC). Logical partitions on slot
# b, group qti_dynamic_partitions_b, in this order (each extent stays where it is as long as the ones before it keep
# their sizes, so a partial write-super.sh only rewrites what changed):
#   system_b, vendor_b, sys_prod_b, chip_prod_b, preload_b, cust_b   the donor's images, each exactly its size
#                                                                   (donor/pura70proplus-7.0.0.107/images and
#                                                                   preload-pkg/x/preload.img)
#   asteroids_b   256 MiB, holding asteroids.img (build/build-asteroids.sh): the M2 bundle, which changes most
#   android_b     the rest of the group, holding android.img (platform/qcom/android/build-android-helpers.sh)
# The metadata is otherwise the stock one (super_empty.img from the device's build: LP 10.2, 64 KiB metadata, 3 slots,
# virtual A/B, groups qti_dynamic_partitions_{a,b} of 9659482112 bytes, super 9663676416 bytes, 1 MiB alignment).
#
# Output in out/super/: super-head.img (the first MiB of super: reserved bytes, geometry and every metadata slot;
# the first extent starts at 1 MiB) and layout.txt ("<name> <first sector> <sectors>" per partition), which
# write-super.sh takes on the phone. Needs lpmake and lpdump (Android's liblp tools).
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd $H/../.. && pwd)
O=$R/out/super
. $R/scripts/conf.sh
D=donor/pura70proplus-7.0.0.107
G=9659482112
A=$((256 << 20))
mkdir -p $O
size() { pc "stat -c %s $1" | tr -dc 0-9; }
args=()
used=0
for p in system vendor sys_prod chip_prod preload cust asteroids android; do
    case $p in
    preload) s=$(size $D/preload-pkg/x/preload.img);;
    asteroids) s=$A; [ $(size m2/asteroids.img) -le $A ] || { echo "asteroids.img is over 256 MiB"; exit 1; };;
    # What's left of the group after 1 MiB alignment of each extent.
    android) s=$(( (G - used) / 1048576 * 1048576 - 8 * 1048576 ))
             [ $(size android-b/android.img) -le $s ] || { echo "android.img is over $s bytes"; exit 1; };;
    *) s=$(size $D/images/$p.img);;
    esac
    [ -n "$s" ] && [ $((s % 4096)) -eq 0 ] || { echo "$p: bad size '$s'"; exit 1; }
    used=$((used + (s + 1048575) / 1048576 * 1048576))
    args+=(--partition ${p}_b:readonly:$s:qti_dynamic_partitions_b)
done
rm -f $O/super-full.img
lpmake --metadata-size 65536 --metadata-slots 3 --device-size 9663676416 --super-name super --virtual-ab \
    --group qti_dynamic_partitions_a:$G --group qti_dynamic_partitions_b:$G "${args[@]}" \
    --force-full-image --output $O/super-full.img 2>/dev/null
lpdump $O/super-full.img > $O/lpdump.txt
# "super: 2048 .. 7881304: system_b (7879256 sectors)" -> "system_b 2048 7879256"
sed -n 's/^super: \([0-9]*\) \.\. [0-9]*: \([a-z_]*\) (\([0-9]*\) sectors)$/\2 \1 \3/p' $O/lpdump.txt > $O/layout.txt
[ $(wc -l < $O/layout.txt) -eq 8 ] || { echo "layout: expected 8 partitions"; cat $O/layout.txt; exit 1; }
awk '$2 < 2048 || $2 % 2048 { bad = 1 } END { exit bad }' $O/layout.txt || { echo "an extent isn't 1 MiB aligned"; exit 1; }
head -c 1048576 $O/super-full.img > $O/super-head.img
rm $O/super-full.img
cat $O/layout.txt
(cd $O && sha256sum super-head.img layout.txt > SHA256SUMS)
