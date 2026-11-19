#!/bin/bash
# DroiTong (com.droi.tong, 卓易通: Huawei's front end for running Android apps in its Android container) re-signed with
# our developer cert, with ohos.permission.ACCESS_FUSION_MANAGER added to its manifest. The container manager
# (hmos_fusion_manager, SA 66854) serves only callers holding that permission or allow-listed Huawei apps checked by
# signature; a re-signed DroiTong is neither, so every call failed (1003) and it said the system version isn't
# supported. sign-hap.sh puts every requested permission in the profile's allowed ACLs, so the added one is granted.
#
#   sign-droitong.sh <droitong.hap>   (a decrypted DroiTong HAP, e.g. 1.0.10.93; output $HARMONY_BUILD/apps/droitong-signed.hap)
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
B=$HB
D=$B/apps/droitong
mkdir -p $D
python3 - "$1" $D/droitong-fusion.hap <<'EOF'
import json, sys, zipfile
src, out = sys.argv[1:3]
zin = zipfile.ZipFile(src)
m = json.loads(zin.read("module.json"))
perms = m["module"].setdefault("requestPermissions", [])
if "ohos.permission.ACCESS_FUSION_MANAGER" not in {p["name"] for p in perms}:
    perms.append({"name": "ohos.permission.ACCESS_FUSION_MANAGER"})
data = json.dumps(m, ensure_ascii=False, separators=(",", ":")).encode()
with zipfile.ZipFile(out, "w") as zout:
    for info in zin.infolist():
        z = zipfile.ZipInfo(info.filename, info.date_time)
        z.compress_type = info.compress_type
        z.external_attr = info.external_attr
        zout.writestr(z, data if info.filename == "module.json" else zin.read(info.filename))
EOF
# As a system app, like Huawei ships it: bms_broker answers DroiTong's query for the Android app store's activity only
# for system apps, SAs or the shell (IsOHCallerAllowed); as a normal app every launch failed with 16000001 ("Target
# environment loading error").
SYSTEM_APP=1 bash $B/repo/common/signing/sign-hap.sh $D/droitong-fusion.hap $B/apps/droitong-signed.hap
