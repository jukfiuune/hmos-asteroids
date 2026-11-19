#!/bin/sh
# Writes the HarmonyOS layout to super (run on the phone, in M1's root shell):
#   write-super.sh <dir> [partition...]
# <dir> holds super-head.img and layout.txt from make-super.sh and an image for each partition to write, named
# without the slot (system.img, asteroids.img, ...). With no partitions named, all eight are written (a fresh super).
# 1. Checks every image fits its extent, and that each partition not written keeps its extent in the new layout
#    (compared with lpmap -l of the current metadata), so its contents stay valid.
# 2. Copies each image into its extent (the rest of the extent is zeroed), then the metadata over the first MiB.
# 3. Checks the metadata now on disk against layout.txt (lpmap -l), and each written partition against its image
#    (SHA-256, read from super at its extent).
# M1 maps super read-only at boot; writing it under a running system is safe, and the system sees the new contents
# after a reboot.
set -e
D=${1:?usage: write-super.sh <dir> [partition...]}
shift
LPMAP=${LPMAP:-lpmap}
S=
for u in /sys/class/block/*/uevent; do grep -q '^PARTNAME=super$' $u && S=/dev/$(basename $(dirname $u)); done
[ -b "$S" ] || { echo "no super partition"; exit 1; }
[ $(cat /sys/class/block/${S#/dev/}/size) -eq 18874368 ] || { echo "$S isn't 9663676416 bytes"; exit 1; }
grep -q "^$S " /proc/mounts && { echo "$S is mounted"; exit 1; }
W=" $* "
[ "$W" = "  " ] && W=" $(cut -d' ' -f1 $D/layout.txt | sed 's/_b$//' | tr '\n' ' ') "

$LPMAP -l $S > /tmp/super-now.txt 2>/dev/null || : > /tmp/super-now.txt
while read name start sectors; do
    p=${name%_b}
    case "$W" in
    *" $p "*)
        [ $(stat -c %s $D/$p.img) -le $((sectors * 512)) ] || { echo "$p.img is larger than $name"; exit 1; };;
    *)
        grep -qx "$name $start+$sectors" /tmp/super-now.txt ||
            { echo "$name moves in the new layout: write it too"; exit 1; };;
    esac
done < $D/layout.txt

while read name start sectors; do
    p=${name%_b}
    case "$W" in *" $p "*) ;; *) continue;; esac
    echo "writing $name at sector $start"
    dd if=$D/$p.img of=$S bs=1M seek=$((start / 2048)) conv=fsync 2>/dev/null
    size=$(stat -c %s $D/$p.img)
    pad=$((sectors * 512 - size))
    [ $pad -gt 0 ] && dd if=/dev/zero of=$S bs=4096 seek=$(((start * 512 + size) / 4096)) count=$((pad / 4096)) \
        conv=fsync 2>/dev/null
done < $D/layout.txt
dd if=$D/super-head.img of=$S bs=1M count=1 conv=fsync 2>/dev/null
sync
echo "metadata written"

rc=0
$LPMAP -l $S > /tmp/super-new.txt
while read name start sectors; do
    grep -qx "$name $start+$sectors" /tmp/super-new.txt || { echo "$name: metadata on disk differs from layout.txt"; rc=1; }
    p=${name%_b}
    case "$W" in *" $p "*) ;; *) continue;; esac
    size=$(stat -c %s $D/$p.img)
    a=$(sha256sum < $D/$p.img | cut -d' ' -f1)
    b=$(dd if=$S bs=4096 skip=$((start / 8)) count=$((size / 4096)) 2>/dev/null | sha256sum | cut -d' ' -f1)
    [ "$a" = "$b" ] && echo "$name: matches $p.img" || { echo "$name: DIFFERS from $p.img"; rc=1; }
done < $D/layout.txt
exit $rc
