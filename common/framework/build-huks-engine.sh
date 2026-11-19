#!/bin/bash
# Builds OpenHarmony's software HUKS key engine as a stand-in for the donor's libhuks_ca_hdi.z.so.
#
#   build-huks-engine.sh <out .so>        (on the build PC, from $HARMONY_BUILD/m2)
#
# The donor's HUKS HDI (vendor/lib64/passthrough/libhuks_hdi_service_1.2.z.so) links libhuks_ca_hdi.z.so, a client of
# Huawei's iTrustee TEE (libteec_vendor -> teecd), which doesn't exist on a Snapdragon. Every call fails, huks_service
# never publishes SA 3510, and every app or service that keeps a key in HUKS (Weather decrypting its API key, apps'
# startup SDKs, asset_service) gets "SA:3510 not start". libhuks_ca_hdi exports flat HuksHdi* functions (version node
# 1.0); upstream's software engine (security_huks huks_engine/main/core, hks_core_interfaces.c) exports the same names
# with the same signatures, so it is built here under that soname. Keys are wrapped with a random root key kept by
# upstream's RKC (root key component) files in /data/data/huks_service, which huks_service.cfg already creates. No
# hardware protection: a software keystore, as an OpenHarmony board without a TEE has.
set -e
. "$(dirname "$0")/../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
OUT=$1
H=$HB/ohos/src/security_huks
N=$HB/ohos/sdk/native
L=$WORK/ohos-link
W=$WORK/out-huks
DONOR=$HB/donor/pura70proplus-7.0.0.107/mnt/system/system
mkdir -p $W/obj
for f in chipset-sdk-sp/libcrypto_openssl.z.so chipset-sdk-sp/libsec_shared.z.so chipset-pub-sdk/libhilog.so; do
    [ -f $L/$(basename $f) ] || sudo -n cat $DONOR/lib64/$f > $L/$(basename $f)
done

F=$H/frameworks/huks_standard/main
E=$H/services/huks_standard/huks_engine/main
SV=$H/services/huks_standard/huks_service/main
SRC="
$E/core/src/hks_auth.c $E/core/src/hks_core_interfaces.c $E/core/src/hks_core_service_key_attest.c
$E/core/src/hks_core_service_key_generate.c $E/core/src/hks_core_service_key_operate_one_stage.c
$E/core/src/hks_core_service_key_operate_three_stage.c $E/core/src/hks_core_service_key_other.c
$E/core/src/hks_core_service_three_stage.c $E/core/src/hks_keyblob.c $E/core/src/hks_keynode.c
$E/core/src/hks_secure_access.c $E/core/src/hks_sm_import_wrap_key.c $E/core/src/hks_upgrade_key.c
$E/device_cert_manager/src/dcm_asn1.c $E/device_cert_manager/src/dcm_attest.c
$E/device_cert_manager/src/dcm_attest_utils.c
$F/common/src/hks_ability.c $F/common/src/hks_base_check.c $F/common/src/hks_check_paramset.c
$F/common/src/hks_common_check.c $F/common/src/hks_crypto_adapter.c $F/common/src/hks_error_msg.c
$F/common/src/hks_param.c $F/common/src/hks_tags_type_manager.c $F/common/src/hks_errcode_adapter.c
$F/crypto_engine/crypto_common/src/hks_core_ability.c $F/crypto_engine/crypto_common/src/hks_core_get_main_key.c
$F/os_dependency/posix/hks_mem.c $F/os_dependency/posix/hks_util.c
$H/utils/crypto_adapter/hks_client_service_adapter.c $H/utils/crypto_adapter/hks_client_service_adapter_common.c
$H/utils/file_operator/hks_file_operator.c $H/utils/list/hks_double_list.c $H/utils/mutex/hks_mutex.c
$F/crypto_engine/rkc/src/hks_rkc.c $F/crypto_engine/rkc/src/hks_rkc_rw.c $F/crypto_engine/rkc/src/hks_rkc_v1.c
$SV/os_dependency/posix/hks_rwlock.c
$SV/hks_storage/src/hks_lock.c $SV/hks_storage/src/hks_storage.c $SV/hks_storage/src/hks_storage_file_lock.c
$SV/hks_storage/src/hks_storage_manager.c $SV/hks_storage/src/hks_storage_utils.c
$F/os_dependency/sysinfo/src/hks_get_process_info_passthrough.c $F/os_dependency/sysinfo/src/hks_get_udid.c
"
for s in 3des ability aes bn common curve25519 des dh dsa ecc ed25519tox25519 engine hash hmac kdf rsa sm2 sm3 sm4; do
    SRC="$SRC $F/crypto_engine/openssl/src/hks_openssl_$s.c"
done
SRC="$SRC $(dirname $0)/huks_engine_extra.c"

# Every header directory in the tree (the GN targets' include_dirs, without tracking them per file), the lite and
# test ones left out, plus OpenSSL, securec and hilog. The OpenSSL headers are the build PC's 3.x ones (the vendored
# 3.0.9 tree lacks its generated headers), as useriam/build-useriam.sh does; -z defs catches anything the donor's
# libcrypto_openssl (3.0.9) doesn't have.
INC=$(find $H/frameworks $H/services $H/utils $H/interfaces -name '*.h' -not -path '*/test/*' -not -path '*lite*' \
      -printf '-I%h\n' | sort -u)
mkdir -p $W/openssl-include && cp -r /usr/include/openssl $W/openssl-include/
INC="$INC -I$W/openssl-include -I$H/../third_party_bounds_checking_function/include
     -I$H/../hiviewdfx_hilog/interfaces/native/innerkits/include"
DEF="-DHKS_KEY_VERSION=3 -D_HUKS_LOG_ENABLE_ -DL2_STANDARD -DHKS_ENABLE_CLEAN_FILE -DSUPPORT_STORAGE_BACKUP
     -DHKS_ENABLE_UPGRADE_KEY -DHKS_CHANGE_DERIVE_KEY_ALG_TO_HKDF -DHKS_USE_RKC_IN_STANDARD
     -DHKS_CONFIG_KEY_STORE_PATH=\"/data/service/el1/public/huks_service\"
     -DHKS_CONFIG_RKC_STORE_PATH=\"/data/data/huks_service\" -DHKS_ENABLE_UPGRADE_RKC_DERIVE_ALG
     -DOPENSSL_SUPPRESS_DEPRECATED -DLOG_DOMAIN=0xD002F00 -DLOG_TAG=\"HUKS\""
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -fPIC -std=gnu11 -w"
objs=()
for s in $SRC; do
    o=$W/obj/$(basename $s .c).o
    $CC $DEF $INC -c $s -o $o
    objs+=($o)
done
$CC -shared -fuse-ld=lld -Wl,-soname,libhuks_ca_hdi.z.so -Wl,--version-script=$(dirname $0)/huks_engine.map \
    -Wl,-z,relro,-z,now -Wl,-z,defs "${objs[@]}" $L/libcrypto_openssl.z.so $L/libsec_shared.z.so $L/libhilog.so -o $OUT
echo "$OUT: $(llvm-nm -D --defined-only $OUT 2>/dev/null | grep -c HuksHdi) HuksHdi exports"
