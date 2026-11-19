# The Android container (Huawei's anco, run by iSulad; DroiTong is its front end). The container image can't be
# downloaded (that needs Huawei device attestation), so it is preinstalled from a preset package staged on /data by
# common/apps/stage-anco.sh: the fusion manager installs it from /sys_prod/opt/virt_service/rgm_hmos as it would a
# factory preset. Skipped while nothing is staged. The platform's step adds the guest's GPU driver (prep.d/85-anco-*).
ANCO=/data/asteroids/anco
RG=opt/virt_service/rgm_hmos
if [ -f "$ANCO/preset/$RG/anco_hmos.img" ]; then

# A normal boot reason, so the fusion manager takes its normal path (its abnormal-reboot path reorders the install
# and the start and climbs a counter). 30-configs.sh has written these files; HarmonyOS init turns them into params.
if [ -f /hmos-ram/cmdline ] && ! grep -q 'ohos.boot.reboot_reason' /hmos-ram/cmdline; then
    c=$(cat /hmos-ram/cmdline); printf '%s ohos.boot.reboot_reason=normal\n' "$c" > /hmos-ram/cmdline
fi
if [ -f /hmos-ram/bootconfig ] && ! grep -q 'ohos.boot.reboot_reason' /hmos-ram/bootconfig; then
    printf 'ohos.boot.reboot_reason = "normal"\n' >> /hmos-ram/bootconfig
fi
rm -f /data/service/el1/public/hmos_fusion_manager/abnormal_reboot.txt
# Once version.json records the image as installed, later boots skip the preset install and with it the rootfs mount
# the version check reads (image version 0/0, StartContainer fails with 1005). Until we find what mounts an installed
# image on a normal boot, every boot takes the fresh-install path, which does (it doesn't rewrite the image).
rm -f /data/service/el1/public/hmos_fusion_manager/version.json

# The preset package under /sys_prod/opt/virt_service/rgm_hmos: a read-only overlay with the package as the upper
# lower layer. 30-configs.sh has bound files over the donor's sys_prod, which the overlay would hide: they are
# captured first and bound back on top.
SPB="etc/bootanimation/bootanimation_custom_config.json etc/oobe etc/window/resources/display_manager_config.xml \
etc/graphic/hgm_policy_config.xml etc/soc_perf/socperf_resource_config.xml etc/soc_perf/socperf_boost_config.xml \
etc/camera/CameraProfiles.json etc/param/hw_defaults.para"
CAP=/hmos-ram/anco-sysprod
for d in $SPB; do
    [ -e "$R/sys_prod/$d" ] || continue
    mkdir -p "$(dirname "$CAP/$d")"
    if [ -d "$R/sys_prod/$d" ]; then mkdir -p "$CAP/$d"; else : > "$CAP/$d"; fi
    mount --bind "$R/sys_prod/$d" "$CAP/$d" || log "anco: capturing sys_prod/$d failed"
done
mkdir -p /hmos-ram/anco-sysprod-lower
mount --bind "$R/sys_prod" /hmos-ram/anco-sysprod-lower
# Android's identity: this phone (Nothing Phone (3a), codename Asteroids, model A059) running a build of its own
# (asteroids_anco), not the Huawei phone the image comes from. Google certifies no Huawei phone; this build isn't
# certified either, and its uncertified-device registration is meant for builds like it. Huawei's hw_product props set
# brand, manufacturer, device and marketing name early, so the layer over the preset carries a copy with ours;
# param_to_prop.cfg below stops HarmonyOS passing in its own.
ID=/hmos-ram/anco-ident
LP=anco_spec/hw_product/etc/prop/local.prop
mkdir -p $ID
if [ -f /hmos-ram/anco-sysprod-lower/$LP ]; then
    mkdir -p $ID/${LP%/*}
    grep -vE '^ro\.(product\.(brand|device|manufacturer|model|name)|config\.marketing_name|build\.(fingerprint|tags))=' \
        /hmos-ram/anco-sysprod-lower/$LP > $ID/$LP
    cat >> $ID/$LP <<'P'
ro.product.brand=Nothing
ro.product.manufacturer=Nothing
ro.product.device=Asteroids
ro.product.name=asteroids_anco
ro.product.model=A059
ro.config.marketing_name=Nothing Phone (3a)
ro.build.fingerprint=Nothing/asteroids_anco/Asteroids:16/BP2A.250605.031.A3/104.5.5.111DHSP3:user/release-keys
ro.build.tags=release-keys
P
fi
if mount -t overlay -o lowerdir=$ID:$ANCO/preset:/hmos-ram/anco-sysprod-lower overlay "$R/sys_prod"; then
    for d in $SPB; do
        [ -e "$CAP/$d" ] && [ "$d" != etc/param/hw_defaults.para ] && mount --bind "$CAP/$d" "$R/sys_prod/$d"
    done
    # The preset parameters (the donor's are preset_enable=false; const parameters are first-write).
    if [ -f $CAP/etc/param/hw_defaults.para ]; then
        grep -v '^const.app_comp.image.preset_' $CAP/etc/param/hw_defaults.para > /hmos-ram/hw_defaults-anco.para
        cat >> /hmos-ram/hw_defaults-anco.para <<'P'
const.app_comp.image.preset_type=2
const.app_comp.image.preset_enable=true
const.app_comp.image.preset_version=PHGSI-LGRP1-CHN 104.5.5.111DH(SP3)
const.app_comp.image.privacy_version=1402
P
        mount --bind /hmos-ram/hw_defaults-anco.para "$R/sys_prod/etc/param/hw_defaults.para"
    fi
    log "anco: preset package on /sys_prod"
else
    log "anco: preset overlay on /sys_prod failed"
fi

# The guest's keymaster (keymaster 3.0 with Huawei's keystore_anco module) works through iTrustee, Kirin's TEE, which
# this phone doesn't have: every key failed with TEEC_ERROR_GENERIC (Instagram can't log in without keys). With the
# host's const.tee.hwkeymaster.enable off, the guest gets ro.tee.anco.hwkeymaster.enbale=false and its keymaster uses
# AOSP's software-only implementation.
KP=$R/system/etc/param/keymaster.para
if [ -f $KP ]; then
    sed 's/^const\.tee\.hwkeymaster\.enable *= *true/const.tee.hwkeymaster.enable = false/' $KP > /hmos-ram/keymaster-anco.para
    mount --bind /hmos-ram/keymaster-anco.para $KP || log "anco: keymaster.para not bound"
fi

# Android's init takes some of its identity from HarmonyOS parameters (param_to_prop.cfg, over /dev/PropBridge):
# brand, manufacturer, device, name, model and marketing name. Those lines go, so Android keeps its own (above).
PP=$R/system/etc/param/param_to_prop.cfg
if [ -f $PP ]; then
    grep -vE '^const\.(product\.(model|name|brand|manufacturer|device_authentication)|software\.model)=' $PP > /hmos-ram/param_to_prop-anco.cfg
    mount --bind /hmos-ram/param_to_prop-anco.cfg $PP || log "anco: param_to_prop.cfg not bound"
fi

# The container's mobile data link is a veth pair named after the phone's data interface: host "anco" + iface, guest
# iface. Huawei's modems call it rmnet0; this phone's is rmnet_data0, and Huawei's container engine
# (librgm_engine_plugin_server.z.so) can't take that name, so Android had no mobile data link:
# - CheckRgmNetParaLegality and DeleteRgmNetwork accept only letters and digits in veth names ("host Veth Name is
#   invalid"). Each of their three name loops ends in "cset wN, lo" (a letter or digit); "mov wN, #1" accepts any name
#   HarmonyOS's network manager hands it.
# - Its iptables chains are a prefix, "_" and the host veth name, and iptables takes 28 characters at most
#   (rgm_nat_POSTRT_ancormnet_data0 failed, and with it the NAT and the routing rules). The prefixes get shorter:
#   rgm_nat_POSTRT -> rgm_nat_POST and rgm_ula_nat_POSTRT -> rgm_ula_POST (read with strlen), ttl_mod_FORWARD ->
#   ttl_mod_FWD (copied as 15 bytes with its length, 30 = 15 << 1, which becomes 22 = 11 << 1).
# Each patch is offset:old:new in hex, and all old bytes are checked first, so another build is left alone.
EP=$R/system/lib64/librgm_engine_plugin_server.z.so
EPP=/hmos-ram/librgm_engine_plugin_server.z.so
EPATCH="5ba98:f5279f1a:35008052 84edc:f6279f1a:36008052 84fe4:f5279f1a:35008052
    219da:5254:0000 219e5:6e61745f504f5354525400:504f535400000000000000 21af5:464f525741524400:4657440000000000
    85808:c9038052:c9028052"
if [ -f $EP ]; then
    cp $EP $EPP
    ok=1
    for p in $EPATCH; do
        o=${p%%:*}; old=${p#*:}; old=${old%:*}
        [ "$(od -An -tx1 -j $((0x$o)) -N $((${#old} / 2)) $EPP | tr -d ' \n')" = "$old" ] || { ok=0; break; }
    done
    if [ $ok = 1 ]; then
        for p in $EPATCH; do
            o=${p%%:*}; new=${p##*:}; bytes=
            while [ -n "$new" ]; do bytes="$bytes\\$(printf %o 0x${new%${new#??}})"; new=${new#??}; done
            printf "$bytes" | dd of=$EPP bs=1 seek=$((0x$o)) conv=notrunc 2>/dev/null
        done
        mount --bind $EPP $EP || log "anco: engine plugin not bound"
        chroot $R /system/bin/chcon u:object_r:system_lib_file:s0 ${EP#$R} || log "anco: engine plugin not labelled"
    else
        log "anco: engine plugin is another build, its veth names not patched"
    fi
fi

# The partitions the rgm engine installs into and verifies against: it writes the image into anco_hmos
# (HvbWriteToPartition) and checks it against rvt (the image carries its own HVB cert; rvt holds its key). We have
# neither, so loop devices over files on /data stand in, linked into ueventd's by-name directory, and /anco_hmos is
# mounted from it as Huawei's fstab would (without hvb).
BN="$R$UFS_BY_NAME"
mkdir -p "$BN" "$R/dev/block"
ln -sfn $UFS_BY_NAME "$R/dev/block/by-name"
L=$(losetup -f) && losetup "$L" $ANCO/anco_hmos.part && ln -sf /dev/block/${L#/dev/} "$BN/anco_hmos" &&
    mount -t erofs -o ro "$L" "$R/anco_hmos" || log "anco: anco_hmos not mounted"
L=$(losetup -f) && losetup -r "$L" $ANCO/rvt.img && ln -sf /dev/block/${L#/dev/} "$BN/rvt" || log "anco: no rvt"

# The container config requires Huawei's hwlog log devices (SourceMustExist); hwlog.ko (common/kernel/hwlog, loaded
# with the vendor_dlkm modules) provides stand-ins, which the guest copies by number. Without it, /dev/null would do
# for the writers, but the guest's hiview_anco reads hwlog_jank in a loop and spins a core on /dev/null's end-of-file.
for n in hwlog_exception hwlog_jank hwlog_dubai; do
    [ -c "$R/dev/$n" ] || { mknod -m 0666 "$R/dev/$n" c 1 3; log "anco: no hwlog.ko, $n on /dev/null"; }
done

# The container's mount overrides (in the root's RAM upper, so they reset every boot):
OV=$R/system/variant/phone/base/etc/anco/anco_override_hostconfig.json
# HongMeng keeps tty info under /proc/devhost/root/tty, Linux in /proc/tty (iSulad refuses a missing bind source).
sed -i 's|"/proc/devhost/root/tty"|"/proc/tty"|' $OV
# Our /dev/binder and /dev/hwbinder are binderfs nodes, which have no char device behind their numbers: the copies
# iSulad mknods in the guest open as ENXIO. The nodes themselves are bound in instead (the same binder context).
grep -q '"/dev/binderfs/hwbinder"' $OV || sed -i 's|"Mounts": \[|"Mounts": [\
    {"Type": "bind", "Source": "/dev/binderfs/binder", "Target": "/dev/binder", "SourceMustExist": true},\
    {"Type": "bind", "Source": "/dev/binderfs/hwbinder", "Target": "/dev/hwbinder", "SourceMustExist": true},|' $OV
# Google Play, when staged (common/apps/stage-anco-gapps.sh): its trees over the guest's directories (each replaces a
# whole directory; the guest's /product and /system_ext live in /system), and /product's build.prop with the Play
# icons let onto the home screen.
GA=$ANCO/gapps
if [ -d $GA ] && ! grep -q "\"$GA/" $OV; then
    for d in product/priv-app product/etc/permissions product/etc/sysconfig product/overlay system_ext/priv-app system_ext/etc \
            product/etc/build.prop; do
        [ -e $GA/$d ] && sed -i "s|\"Mounts\": \[|\"Mounts\": [\\
    {\"Type\": \"bind\", \"Source\": \"$GA/$d\", \"Target\": \"/system/$d\", \"SourceMustExist\": true},|" $OV
    done
    log "anco: Google Play bound into the guest"
fi
# HarmonyOS's BMS broker hid the Play Store's icon for the user when it first mirrored the app (the guest had asked
# it to) and keeps that in its database, though it checks again only on an install: cleared here, before it starts.
DB=/data/service/el1/public/bundle_framework_broker/bundle_framework_broker.db
if [ -d $GA ] && [ -f $R$DB ]; then
    chroot $R /system/bin/sqlite3 $DB "update installed_bundle set VALUE = replace(VALUE,
        '\"hideDesktopIconUserIds\":[100]', '\"hideDesktopIconUserIds\":[]') where KEY = 'com.android.vending'" ||
        log "anco: Play Store icon state not cleared"
fi
# The guest's init makes its service cgroups under /sys/fs/cgroup, which iSulad's default spec mounts read-only.
for f in config.json hm_config.json systemcontainer_config.json hm_systemcontainer_config.json; do
    F=$R/system/etc/virt_service/rgm_engine/isula/etc/default/isulad/$f
    [ -f $F ] && sed -i '/"destination": "\/sys\/fs\/cgroup"/,/"ro"/ s/"ro"/"rw"/' $F
done
# iSulad's lxc only accepts a cgroup2 hierarchy: one at /sys/fs/cgroup/unified, once HarmonyOS init (iSulad.cfg) has
# put its tmpfs on /sys/fs/cgroup.
# Then a watcher on the container's cgroup. When the screen goes off, the low-power manager pauses the container. iSulad's lxc freezes
# the cgroup, then fails on HongMeng's deep freeze (/dev/frz/isulad/<id>/anco_deep_frozen, hm.freezer.pin_state) and
# reports the pause failed ("runtime errno -205") but leaves the freeze. Everyone then takes the container as
# running, nobody resumes it, and every Android app hangs. Just before the pause, the guest's own freezer (hwpged)
# has also frozen each app's processes in a /dev/frz group (the host's freezer hierarchy), and it thaws them only on a
# resume. So any container freeze is undone, along with the frozen freezer groups that hold container processes
# (HarmonyOS's own app groups stay as they are). That is the state HarmonyOS believes in. Not at once: lxc waits on
# cgroup.events for "frozen 1" before its deep-freeze step, and a thaw before it reads that left it waiting forever
# holding the container's lock (iSulad then answered every query with "trylock timeout", and the network broker
# took the container for gone). The watcher waits for "frozen 1" and gives lxc 2 s to finish first. Each thaw is
# logged to /data/local/tmp/anco-thaw.txt.
nohup sh -c 'U=/proc/1/root/sys/fs/cgroup/unified; i=0; while [ $i -lt 240 ]; do
        if [ -d /proc/1/root/sys/fs/cgroup/devices ]; then
            mkdir -p $U && mount -t cgroup2 none $U
            break
        fi; sleep 0.5; i=$((i + 1)); done
    while :; do
        C=$(ls -d $U/isulad/*/ 2>/dev/null | head -1)
        if [ -z "$C" ]; then sleep 5; continue; fi
        echo 0 > ${C}cgroup.freeze
        inotifyd - ${C}cgroup.events:c | while read -r e f; do
            [ "$(cat ${C}cgroup.freeze 2>/dev/null)" = 1 ] || continue
            grep -q "frozen 1" ${C}cgroup.events || continue
            sleep 2
            [ "$(cat ${C}cgroup.freeze 2>/dev/null)" = 1 ] || continue
            echo 0 > ${C}cgroup.freeze
            n=0
            for g in /proc/1/root/sys/fs/cgroup/freezer/Group_*; do
                [ "$(cat $g/freezer.state 2>/dev/null)" = FROZEN ] || continue
                p=$(head -n 1 $g/cgroup.procs)
                [ -n "$p" ] && grep -q "::/isulad/" /proc/$p/cgroup 2>/dev/null &&
                    echo THAWED > $g/freezer.state && n=$((n + 1))
            done
            echo "$(date "+%m-%d %T") thawed ${C#$U/} and $n app groups" >> /data/local/tmp/anco-thaw.txt
        done
        sleep 5
    done' </dev/null >/dev/null 2>&1 &
log "anco: container prepared"

fi
