#!/bin/bash
# Builds the modem's file services for M1/prep.sh, static against the OpenHarmony NDK's musl (like pd-mapper):
#   rmtfs      Linaro's remote file system server: the modem's EFS in modemst1/modemst2/fsg/fsc (QMI service 14),
#              patched for this kernel's msm_sharedmem UIO (patch-rmtfs.py).
#   tqftpserv  Linaro's QRTR TFTP server: the modem reads its configs (MBNs) from the modem partition's image/
#              and keeps small files in a read-write directory.
#   asteroids_atd  the AT proxy between HarmonyOS's RIL vendor library and the modem (asteroids_atd.c).
#   asteroids_uim  activates the SIM cards' subscriptions over QMI UIM (asteroids_uim.c).
#   asteroids_voicelog  a diagnostic: logs the modem's voice call events and end reasons (asteroids_voicelog.c).
#   asteroids_data  a cellular data call: IPA pipes, rmnet_data0 and QMI WDA/WDS (asteroids_data.c).
# Sources are fetched at pinned commits into $HARMONY_BUILD/modem (run on the build PC).
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
N=$HB/ohos/sdk/native
W=$HB/modem
H=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$W/out}
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wno-unused-parameter -fuse-ld=lld -static"

fetch() {  # name commit
    [ -d $W/$1 ] || git clone -q https://github.com/linux-msm/$1 $W/$1
    git -C $W/$1 checkout -q -f $2
}
mkdir -p $W $OUT
fetch qrtr 29e36ae164389580a0f8ea7a7fdb728140ae978d
fetch rmtfs b30a3eb38f9af283f18dbd3c7755653efc52c094
fetch tqftpserv eb4b9a04ff92d2753ededf32cc3b6ed287f6e29b

# linux/qrtr.h from pd-mapper (bionic's copy: the NDK's pulls in linux/socket.h, which clashes with musl's).
LQ="-I$H/../audio/pd-mapper -I$W/qrtr/include $W/qrtr/lib/qrtr.c $W/qrtr/lib/qmi.c $W/qrtr/lib/logging.c"

(cd $W/rmtfs && python3 $H/patch-rmtfs.py)
$CC -DANDROID -I$W/rmtfs $W/rmtfs/{qmi_rmtfs,rmtfs,rproc,sharedmem,storage,util}.c $LQ -lpthread -o $OUT/rmtfs

# Firmware from the modem partition (mounted at /vendor/firmware_mnt), read-write files in /data/asteroids/modem.
sed -e 's|"/lib/firmware/"|"/vendor/firmware_mnt/image/"|' -e 's|"/var/lib/tqftpserv"|"/data/asteroids/modem"|' \
    $W/tqftpserv/translate.c > $OUT/translate.c
$CC -I$W/tqftpserv $W/tqftpserv/tqftpserv.c $OUT/translate.c $LQ -o $OUT/tqftpserv

# The AT proxy between the RIL vendor library and the modem (a HarmonyOS service, asteroids-desktop.cfg).
$CC -Wextra $H/asteroids_atd.c -o $OUT/asteroids_atd

$CC -Wextra $H/asteroids_uim.c $LQ -o $OUT/asteroids_uim
$CC -Wextra $H/asteroids_voicelog.c $LQ -o $OUT/asteroids_voicelog
$CC -Wextra $H/asteroids_data.c $LQ -o $OUT/asteroids_data
ls -la $OUT/rmtfs $OUT/tqftpserv $OUT/asteroids_atd $OUT/asteroids_uim $OUT/asteroids_voicelog $OUT/asteroids_data
