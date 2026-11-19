#!/bin/bash
# Stages the Android container's preset package on the phone (laptop; the phone booted to HarmonyOS or M1, reached
# with ./port): build-anco-preset.sh's output from the build PC into /data/asteroids/anco, which prep.d/65-anco.sh reads
# each boot. A wiped userdata needs this again; nothing else does.
#   preset/opt/virt_service/rgm_hmos/  anco_hmos.img, config.zip, compatible_version_in.json (the preset package)
#   anco_hmos.part                     the "partition" the rgm engine installs the image into (a copy, looped)
#   rvt.img                            the OTA's verified-boot root table (looped read-only)
# The image is 1.2 GB: it is sent once and copied on the phone.
#   stage-anco.sh [preset dir on the PC]   (relative to BUILD_ROOT; default fw/bre-al80/anco-preset)
set -e
cd "$(dirname "$0")/../.."
SRC=${1:-fw/bre-al80/anco-preset}
. scripts/conf.sh
T=$(mktemp -d)
trap 'rm -rf $T' EXIT
pc_get $T $SRC/{anco_hmos.img,config.zip,compatible_version_in.json,rvt.img,SHA256SUMS}
(cd $T && sha256sum -c --quiet SHA256SUMS)
D=/data/asteroids/anco
P=$D/preset/opt/virt_service/rgm_hmos
./port sh "mkdir -p $P"
for f in anco_hmos.img config.zip compatible_version_in.json; do ./port put $T/$f $P/$f | tail -1; done
./port put $T/rvt.img $D/rvt.img | tail -1
./port sh "cp $P/anco_hmos.img $D/anco_hmos.part && sync && ls -la $D $P" 600
echo "staged; reboot to install"
