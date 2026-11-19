#!/bin/bash
# Extracts the donor's EROFS images on the build PC into donor/pura70proplus-7.0.0.107/x/<partition>/, once, so builds
# can read the donor's files (the configs build-hmos-m2.sh derives ours from) without root or loop mounts.
# Needs erofs-utils (build-erofs-utils.sh).
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
D=$HB/donor/pura70proplus-7.0.0.107
E=$HB/tools/erofs-utils/bin
for p in system vendor sys_prod chip_prod cust; do
    [ -d $D/x/$p ] && continue
    rm -rf $D/x/$p.new && mkdir -p $D/x
    $E/fsck.erofs --extract=$D/x/$p.new $D/images/$p.img >/dev/null
    chmod -R u+rwX $D/x/$p.new
    mv $D/x/$p.new $D/x/$p
done
du -sh $D/x/*
