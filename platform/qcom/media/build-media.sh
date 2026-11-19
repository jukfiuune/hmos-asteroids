#!/bin/bash
# Builds the video pieces on the build PC (OHOS SDK, musl):
#   libOMX_Core.z.so (omx_core.c, v4l2dec.c, v4l2enc.c): the OpenMAX IL core the codec HDI loads, with Qualcomm's
#     msm_video decoder and encoder behind OMX.hisi.video.decoder.avc/.hevc and OMX.hisi.video.encoder.avc/.hevc.
#     Goes over the donor's in /vendor/lib64/passthrough/indirect.
#   native_video_decode_check: decodes a file through the public AVCodec API, as an app would.
#   native_video_encode_check: encodes synthetic frames through the public AVCodec API, as an app would.
#   v4l2_probe, v4l2_decode_test, v4l2_encode_test: msm_video on its own, without HarmonyOS (static).
# Needs OpenHarmony's OpenMAX headers (third_party_openmax, with codec_omx_ext.h) in $HARMONY_BUILD/ohos/src.
# Output: $WORK/out-media/.
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
N=$HB/ohos/sdk/native
OMX=$HB/ohos/src/third_party_openmax/api/1.1.2
O=$WORK/out-media
mkdir -p $O
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld"
$CC -Wno-unused-parameter -shared -fPIC -Wl,-z,defs -Wl,-soname,libOMX_Core.z.so -I$OMX \
    $QCOM/media/omx_core.c $QCOM/media/v4l2dec.c $QCOM/media/v4l2enc.c -pthread -o $O/libOMX_Core.z.so
$CC $QCOM/media/native_video_decode_check.c -lnative_media_vdec -lnative_media_codecbase -lnative_media_core \
    -lnative_media_avdemuxer -lnative_media_avsource -pthread -o $O/native_video_decode_check
$CC -static $QCOM/media/v4l2_probe.c -o $O/v4l2_probe
$CC -static $QCOM/media/v4l2_decode_test.c -o $O/v4l2_decode_test
$CC $QCOM/media/native_video_encode_check.c -lnative_media_venc -lnative_media_codecbase -lnative_media_core -pthread \
    -o $O/native_video_encode_check
$CC -static -I$QCOM/media $QCOM/media/v4l2_encode_test.c $QCOM/media/v4l2enc.c -o $O/v4l2_encode_test
