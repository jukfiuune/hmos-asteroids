# Audio: M1 booted the ADSP without a protection-domain mapper, so the kernel never saw its audio domain come up and
# the sound card never registered. Start the mapper (sheng's; static, it outlives the handoff). With
# the mapper up the audio domain's PDR comes through and the card registers on its own (about 0.2 s). Only if it
# doesn't, restart the ADSP: that works for playback, but the LPASS SoundWire masters don't survive it (lpass core hw
# enable failed) and the codec's TX side stays disconnected, so the microphones read zeros.
L=/data/local/tmp/pd-mapper.log
setsid $A/bin/pd-mapper --maps-dir /vendor/firmware_mnt/image > $L 2>&1 </dev/null &
until_ 30 grep -q '^PD_LOCATOR_PUBLISHED' $L
until_ 50 sh -c '! grep -q "no soundcards" /proc/asound/cards' ||
    { log "audio: no sound card with the mapper up; restarting the ADSP (microphones won't work)"
      r=$(rproc $RPROC_ADSP); echo stop > $r/state && echo start > $r/state || log "audio: ADSP restart failed"; }
