#!/bin/bash
# Builds the anco (DroiTong Android container) preset-install package from a Huawei phone BASE OTA, so the fusion
# manager installs the Android image locally instead of downloading it. DroiTong's online image download fails at
# Huawei device attestation (UcsError 631008: needs a device cert from the TEE; our HUKS is software, no cert), so the
# attested download is a dead end on this port. hmos_fusion_manager has a second path: if an anco preset package is
# present under /sys_prod/opt/virt_service/rgm_hmos/ (isAncoPreset_), it installs from there with no download and no
# attestation. The donor Pura70 sys_prod ships none of that dir, which is exactly why DroiTong fell through to the
# download; this script produces the package the preset path wants, from a genuine (Huawei-signed) BASE OTA.
#
# The three files the preset path reads:
#   anco_hmos.img              the Android container rootfs (EROFS). Comes from the OTA's own anco_hmos PARTITION.
#   config.zip                 rgm host-bridge configs (graphic/camera/multimedia/netmanager/...) + signed fileList.
#   compatible_version_in.json softwarePlat / versionRequire gate (BRE-AL80: 7.0.0 / 70010; our 7.0.0.107 satisfies it).
# config.zip and compatible_version_in.json come from the OTA's sys_prod partition (/opt/virt_service/rgm_hmos/).
# Also carved: rvt.img, the OTA's verified-boot root table. The rgm engine writes the image into an anco_hmos partition
# and verifies it (HVB: the image carries its own cert and footer; rvt holds the anco_hmos public key), so the phone
# needs both as block devices, which we don't have (see the test hook notes in the anco-image memory).
#
# Default source: a BRE-AL80 (HarmonyOS 6, generic_arm_64_anco / Android 16) full BASE OTA. Any Huawei BASE OTA with a
# real anco_hmos partition works; swap the URL. The image is device-agnostic "generic_arm_64_anco", not Kirin-bound.
#
#   build-anco-preset.sh [update_full_base.zip | OTA_URL]
# Output: $B/fw/<tag>/anco-preset/{anco_hmos.img, config.zip, compatible_version_in.json, rvt.img, SHA256SUMS}
#
# Deploy (separate, needs the phone, NOT done here): put anco_hmos.img on /data and bind the three files into
# /sys_prod/opt/virt_service/rgm_hmos/ from a prep.d step (the dir does not exist on the donor, so that step overlays
# /sys_prod or creates the skeleton first). See docs/apps.md "DroiTong / anco preset install".
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
B=$HB
E=$B/tools/erofs-utils/bin
SRC="${1:-http://update.dbankcdn.com/download/data/pub_13/HWHOTA_hota_900_9/72/v3/Q83PEX4VTE6ujqwtkCYmxQ/full/update_full_base.zip}"
TAG=bre-al80
W=$B/fw/$TAG
mkdir -p $W
cd $W

# 1. Get the OTA zip.
if [ -f "$SRC" ]; then
    ZIP="$SRC"
else
    ZIP=$W/update_full_base.zip
    [ -f "$ZIP" ] || curl -fSL -C - -o "$ZIP" "$SRC"
fi

# 2. Pull update.bin out of the zip (stored, large; the payload blob).
[ -f update.bin ] || python3 - "$ZIP" <<'PY'
import zipfile, shutil, sys
z = zipfile.ZipFile(sys.argv[1])
with z.open("update.bin") as f, open("update.bin", "wb") as o:
    shutil.copyfileobj(f, o, 1 << 20)
PY

# 3. Carve the anco_hmos and sys_prod partitions from update.bin (HarmonyOS-5+ L2 format: 178 B header, 87 B records,
#    size at record+47; see tools/unpack_huawei_package). Carve only what we need rather than all 48 partitions.
python3 - <<'PY'
import os, struct
fp = "update.bin"; want = {"anco_hmos", "sys_prod", "rvt"}
with open(fp, "rb") as f:
    f.seek(178); comp_all = struct.unpack("H", f.read(2))[0]
    f.seek(178 + 2 + comp_all + 16); t = struct.unpack("H", f.read(2))[0]
    if t == 0x08:
        hs = struct.unpack("I", f.read(4))[0]; data = 178 + 2 + comp_all + 16 + 2 + 4 + hs
    else:
        f.read(16); hs = struct.unpack("I", f.read(4))[0]; data = 178 + 2 + comp_all + 16 + 18 + 4 + hs
    OFF = 178 + 2; csz = 0
    for i in range(comp_all // 87):
        if i: data += csz
        f.seek(OFF); name = f.read(87).split(b"\x00")[0].decode().lstrip("/")
        f.seek(OFF + 47); csz = struct.unpack("Q", f.read(8))[0]; OFF += 87
        if name in want:
            with open(name + ".img", "wb") as o:
                f.seek(data); rem = csz
                while rem > 0:
                    b = f.read(min(rem, 1 << 20))
                    if not b: break
                    o.write(b); rem -= len(b)
            print("carved %s.img %d bytes" % (name, csz))
PY

# 4. Extract the preset config files from sys_prod (/opt/virt_service/rgm_hmos/).
rm -rf sysprod_x
$E/fsck.erofs --extract=sysprod_x sys_prod.img >/dev/null
P=sysprod_x/opt/virt_service/rgm_hmos
[ -f "$P/config.zip" ] || { echo "ERROR: $P/config.zip not found in this OTA's sys_prod"; exit 1; }

# 5. Assemble the preset package.
O=$W/anco-preset
rm -rf $O && mkdir -p $O
cp -f anco_hmos.img $O/
cp -f $P/config.zip $P/compatible_version_in.json $O/
cp -f rvt.img $O/
( cd $O && sha256sum anco_hmos.img config.zip compatible_version_in.json rvt.img > SHA256SUMS )
echo "=== anco preset package ready: $O ==="
ls -l $O
cat $O/compatible_version_in.json
