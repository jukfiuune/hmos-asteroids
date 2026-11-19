#!/bin/bash
# Builds sheng's audio path (compat/audio, copied to audio/sheng with /compat paths moved to /system) on the build PC.
#   HarmonyOS side (OHOS SDK, musl): libsheng_audio_vdi.so (the render VDI, a client of the PAL server's socket),
#     libsheng_audio_vdi_loader.so (LD_PRELOAD in audio_host/audio_server that swaps the donor's primary VDI and
#     manager factory for ours), libsheng_audio_manager_service.so, native_audio_hdi_publisher (publishes
#     audio_manager_service to the HDF service manager), native_audio_service_launcher (sets the preload and execs).
#   Android side (Bionic, run under Android's linker64 in /android): libsheng_pal_server.so takes over agmplay's
#     __libc_init, starts AGM and PAL and serves @sheng.pal.pcm.v1; libsheng_pal_lifecycle.so keeps PAL's virtual
#     mixer open until pal_deinit has joined its event thread.
#   Capture (SHENG_CAPTURE_EXPERIMENTAL) stays off: its card monitor, pal_register_global_callback, reaches for
#     /dev/hwbinder and aborts the server.
# Needs the PAL headers (CodeLinaro arpal-lx, tag AUDIO.LA.9.0.r1-09100-lanai.0) in $HARMONY_BUILD/src/arpal-lx and the
# phone's Bionic libc/libdl in audio/bionic-link. Output: $WORK/out-audio/.
set -e
. "$(dirname "$0")/../../../build/env.sh"
HB=${HARMONY_BUILD:-$HOME/harmony-build}
M2=$WORK
A=$QCOM/audio/sheng
N=$HB/ohos/sdk/native
S=$HB/ohos/src
O=$WORK/out-audio
mkdir -p $O
CC="$N/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$N/sysroot -O2 -Wall -Wextra -fuse-ld=lld"
HDF=$S/drivers_hdf_core/interfaces/inner_api
$CC -D_GNU_SOURCE -shared -fPIC -Wl,-z,defs -Wl,-soname,libsheng_audio_vdi.so -I$A -I$A/include \
    $A/audio_vdi.c $A/pal_client.c -pthread -o $O/libsheng_audio_vdi.so
$CC -shared -fPIC -Wl,-z,defs $A/audio_vdi_loader.c -ldl -o $O/libsheng_audio_vdi_loader.so
$CC -shared -fPIC -Wl,-z,defs $A/audio_manager_service.c -ldl -pthread -o $O/libsheng_audio_manager_service.so
$CC -I$HDF/ipc -I$HDF/core -I$HDF/utils -I$HDF/osal/shared -I$HDF/osal/uhdf -I$S/drivers_hdf_core/framework/include/utils \
    $A/native_audio_hdi_publisher.c -ldl -o $O/native_audio_hdi_publisher
$CC $A/native_audio_service_launcher.c -o $O/native_audio_service_launcher
# A silent playback check through the public OHAudio API (run as the shell service, see docs/audio.md).
$CC $A/native_system_audio_check.c -lohaudio -o $O/native_system_audio_check
# The same for recording, with a chosen source type (native_audio_rec_check.c).
$CC $QCOM/audio/native_audio_rec_check.c -lohaudio -o $O/native_audio_rec_check
# A test client for the PAL server (root, beside HarmonyOS; static).
$CC -static -I$A $QCOM/audio/pal_tone.c $A/pal_client.c -lm -o $O/pal_tone

BCC="$N/llvm/bin/clang --target=aarch64-linux-android33 --sysroot=$N/sysroot -isystem $N/sysroot/usr/include/aarch64-linux-ohos
     -fuse-ld=lld -fPIC -shared -nostdlib -D_GNU_SOURCE -mno-outline-atomics -O2 -Wall -Wextra -Wl,-z,defs
     -L$WORK/bionic-link"
$BCC -DSHENG_CAPTURE_EXPERIMENTAL=1 -I$A -I$HB/src/arpal-lx/inc -Wl,-soname,libsheng_pal_server.so \
    $A/pal_server.c -lc -ldl -o $O/libsheng_pal_server.so
$BCC -Wl,-soname,libsheng_pal_lifecycle.so $A/pal_lifecycle.c -lc -ldl -o $O/libsheng_pal_lifecycle.so
# Ours: PAL's caller-pid log line without hwbinder (pal_no_hwbinder.c).
$BCC -Wl,-soname,libpal_no_hwbinder.so $QCOM/audio/pal_no_hwbinder.c -lc -o $O/libpal_no_hwbinder.so
ls -l $O
