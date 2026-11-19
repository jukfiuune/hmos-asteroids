#!/bin/sh
# Prepares the HarmonyOS root M1 assembled (/hmos-root) before the handoff: M1 init runs this from the asteroids
# partition. The handoff binds the root onto / as it is and HarmonyOS init makes every service's sandbox namespace from
# it, so whatever HarmonyOS must see goes in here. The steps are prep.d/*.sh in order, sourced (R is the root, A the
# asteroids partition); everything long-running after the handoff is a HarmonyOS init service instead (svc/, started
# by init/asteroids-helpers.cfg).
TAG=prep
. /asteroids/boot/lib.sh
R=${1:-/hmos-root}
A=/asteroids
G=/hmos-ram/asteroids
mkdir -p $G /data/local/tmp
: > /data/local/tmp/asteroids-boot.log
log "started"
for s in /asteroids/boot/prep.d/*.sh; do
    . $s || log "$(basename $s) failed"
done
# Experiments without rebuilding: /data/asteroids/m2-test.sh, if present (sourced; R, A and G are set).
[ -f /data/asteroids/m2-test.sh ] && . /data/asteroids/m2-test.sh
log "done: vendor/etc/init entries $(ls $R/vendor/etc/init | wc -l)"
# The kernel log so far (it wraps within the first minute): module loads, firmware requests, M1.
dmesg > /data/local/tmp/dmesg-handoff.txt
