# Boots the modem (MPSS) the way the Wi-Fi processor is booted: from prep.sh, before the handoff, with no Android
# userspace. prep.d/80-modem.sh runs it after pd-mapper is up (the modem's protection domains register through it).
#   - rmtfs (Linaro's, patched for msm_sharedmem's UIO; build-modem.sh) serves the modem's EFS from modemst1,
#     modemst2, fsg and fsc, found through /dev/disk/by-partlabel. This is what Android's rmt_storage does on every
#     boot: the modem writes its NV back to modemst1/2 as it runs.
#   - tqftpserv (Linaro's) serves the modem partition's image/ read-only (MBN configs) and /data/asteroids/modem.
#   - remoteproc's "start", with recovery on, so a modem crash restarts the modem rather than the phone.
#   - asteroids_uim (asteroids_uim.c) activates the SIM cards' subscriptions once the modem is up.
# The modem then shows its QMI services on QRTR and an AT channel, /dev/at_mdm0 (glink_pkt), which HarmonyOS's RIL
# host uses through the donor's AT vendor library (asteroids-modem.para; ueventd gives the node to radio).
# All three outlive the handoff. Logs: /data/local/tmp/{rmtfs,tqftpserv,asteroids-uim}.log.
TAG=modem
. /asteroids/boot/lib.sh
T=/data/local/tmp

r=$(rproc $RPROC_MODEM)
[ -n "$r" ] || { log "no modem remoteproc"; exit 1; }
[ "$(cat $r/state)" = running ] && { log "already running"; exit 0; }

mkdir -p /dev/disk/by-partlabel /data/asteroids/modem
n=0
for u in /sys/class/block/*/uevent; do
    p=$(sed -n 's/^PARTNAME=//p' $u)
    case " $EFS_PARTS " in *" $p "*)
        ln -sf /dev/$(basename $(dirname $u)) /dev/disk/by-partlabel/$p; n=$((n + 1));;
    esac
done
[ $n = $(echo $EFS_PARTS | wc -w) ] || { log "EFS partitions missing ($n of $(echo $EFS_PARTS | wc -w))"; exit 1; }

setsid /asteroids/bin/rmtfs -P > $T/rmtfs.log 2>&1 </dev/null &
setsid /asteroids/bin/tqftpserv > $T/tqftpserv.log 2>&1 </dev/null &
# rmtfs registers its QMI service right after it maps the shared memory ("using /dev/uioN", patch-rmtfs.py).
i=0
while ! grep -q '^using /dev/' $T/rmtfs.log 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
grep -q '^using /dev/' $T/rmtfs.log || { log "rmtfs didn't start: $(head -3 $T/rmtfs.log | tr '\n' ' ')"; exit 1; }
sleep 0.3

echo enabled > $r/recovery
echo start > $r/state || { log "start failed"; exit 1; }
log "started"
# The SIM cards' subscriptions: the modem waits for the AP to activate them (qcrild's job on Android). asteroids_uim
# waits for the modem's UIM service, activates any card present and stays for cards inserted later.
setsid /asteroids/bin/asteroids_uim > $T/asteroids-uim.log 2>&1 </dev/null &
