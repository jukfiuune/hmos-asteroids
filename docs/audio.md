# Audio

sheng's design: Android's own Qualcomm stack (AGM, PAL, ACDB) runs under Android's linker64 beside HarmonyOS, and
HarmonyOS's audio host reaches it over a unix socket.

## Hardware

Card 0 `volcano-qrd-wsa883x-snd-card`. Speakers: two tfa98xx amps (I2C 13-0034/0035) on `MI2S-LPAIF-RX-PRIMARY`
(48 kHz, 2 ch, 32-bit, PAL device key 0xA2000001). Mic: WCD9378 (handset mic CODEC_DMA-LPAIF_RXTX-TX-3, AMIC1/3 over
the VA SoundWire master). Earpiece. No headphone jack; the card's "Headset Jack" input is the USB-C path through the
FSA4480 switch (untested).

## The Android root (`/android`)

The android partition in super (`platform/qcom/android/build-android-helpers.sh`, [storage.md](storage.md)): the files the
helpers can load out of Android's slot b system, vendor and odm (the dependency closure of the daemons, their HALs,
our preloaded libraries and every library a vendor or odm config names, plus vendor/etc and the DSP modules in
vendor/lib/rfsa), bionic and linker64 from system's com.android.runtime APEX and VNDK 34 libraries from system_ext's
com.android.vndk.v34 APEX; library order vendor:system:vndk34:runtime. `platform/qcom/boot/svc/android-root.sh` (init service asteroids_android)
mounts it at `/android` with `/data/asteroids/android` as its /data, overlays vendor/etc in RAM (sku_volcano mixer_paths and resourcemanager, PAL's context manager
off, `backend_conf.xml`), starts `audioadsprpcd audiopd adsp` (with dsp_b on /vendor/dsp; without it gsl_init fails)
and the PAL server (retried until AGM's gsl_init stops returning -19), then sets `asteroids.audio.start` (audio_host
with sheng's HDI publisher, audio_server with sheng's VDI). It then starts sensors, Bluetooth and RichTap.

- PAL's log has "dlopen failed for Feature Stats": libaudiofeaturestats.so's libqcc_sdk.so wants
  `android::binder::atrace_begin`, which only VNDK 34's libbinder has, and the helpers' library order puts system's
  first. The same order was used with the full Android images, so it failed then too; nothing seen depends on it.
- AGM runs in-process: `platform/qcom/audio/agm_init_preload.c` takes over agmplay's `__libc_init` (as sheng), which is how every
  Android-side server here runs. `pal_no_hwbinder.c` answers PAL's `getCallingPid()` (it otherwise opens
  /dev/hwbinder and aborts).
- prep.sh restarts the ADSP only if no card appears within 5 s: the restart left the VA SoundWire link down and the
  mic read zeros. Don't unbind the wcd9378 SoundWire slaves to test: it hangs init.
- After a panic reboot the ADSP's audio side once didn't come back (AGM -19 ten times); a normal reboot fixes it. The
  sensors no longer wait on audio.

## HarmonyOS side

HarmonyOS OHAudio → audio_server → sheng's VDI (`platform/qcom/audio/sheng`, `build-audio.sh`, PAL headers from arpal-lx
AUDIO.LA.9.0.r1-09100-lanai.0) → PAL server → speaker.

- prep.sh binds a policy config without the `offload_output` pipe (audio_server picked offload for music; the VDI has
  only the normal render), speaker on `primary_output`.
- Levels: PAL streams open at 1.0 (sheng had 0.03, never raised); audio_server applies volume in software, as
  AudioFlinger does.
- Capture uses PAL_STREAM_DEEP_BUFFER (Android's normal record graph with Fluence). `SHENG_CAPTURE_EXPERIMENTAL=1`.
  HarmonyOS's own enhance chains (SCENE_RECORD, SCENE_VOIP_UP) fail ("no enhance chain config info"; Huawei's
  algorithms). Calls get Fluence noise suppression but no echo cancellation.
- Calls: [modem.md](modem.md) (voice stream in the PAL server, routes in the VDI).

## When the link breaks

2026-11-16: audio went silent until a reboot. A stop request's reply came after the client's 2 s timeout (the phone
dozing mid-request, or a slow stop); the VDI gave up without closing the stream, the late reply then answered its next
request, and from there every write was refused (`PAL_SERVER_REQUEST op=3 rc=-22` thousands of times). Now
(`pal_client.c`, `audio_vdi.c`):

- The client skips replies older than its request, and says when the link itself failed (`sheng_pal_call`'s
  `broken`).
- On a failed link the VDI drops the connection (the server closes the stream with it), logs
  `SHENG_AUDIO_LINK_LOST`, and the next Start or write reconnects, reopens and restarts the stream
  (`SHENG_AUDIO_RECOVER`). A write the server refuses because its stream isn't running is recovered the same way.
- The PAL server is restarted if it exits ([boot.md](boot.md)); the VDI reconnects to the new one.

Tested silently: the PAL server stopped for 3 s mid-playback (the write timed out, the VDI recovered and the next
playback went through) and killed outright.

## Checks

`asteroids_audio_check` (writes digital silence), `asteroids_audio_tone` (660 Hz), logs in
`/data/local/tmp/audio-check.txt`; `PAL_SERVER_PCM_SUMMARY` in `/data/local/tmp/pal-server.txt`; `platform/qcom/audio/pal_tone.c`
talks to the PAL socket from M1 (`rec`, `play`, `voice on|off|speaker|earpiece`). Native recording needs
MICROPHONE and MICROPHONE_BACKGROUND in both `permission` and `permission_acls` (`platform/qcom/audio/native_audio_rec_check.c`).
Per the user's rule, no audible test sounds without asking.

## Open

- Microphone from an app (audio_server enumerates the mic port; recorded only through native checks and calls).
- Echo cancellation in calls (PAL's VoIP TX with the playback reference).
- USB-C audio (FSA4480), Bluetooth audio profiles.
