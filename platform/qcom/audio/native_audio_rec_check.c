/* SPDX-License-Identifier: MIT
 * asteroids: records through the public OHAudio capturer API, as an app would, so audio_server's own capture
 * processing (chosen by source type) runs: "native_audio_rec_check <log> <out.wav> [source] [seconds]", source 0 =
 * MIC (a plain recording), 7 = VOICE_COMMUNICATION (a call). Run as an init service (asteroids_audio_rec*), which has
 * the microphone permission. Writes 48 kHz stereo S16 and logs the peak. */
#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *out;
static uint64_t bytes;
static int peak;

static int32_t read_data(OH_AudioCapturer *c, void *user, void *buffer, int32_t length)
{
    (void)c; (void)user;
    int16_t *s = buffer;
    for (int32_t i = 0; i < length / 2; i++) { int v = abs(s[i]); if (v > peak) peak = v; }
    fwrite(buffer, 1, (size_t)length, out);
    __atomic_fetch_add(&bytes, (uint64_t)length, __ATOMIC_RELAXED);
    return 0;
}

static void header(uint32_t data)
{
    unsigned char h[44] = {0};
    uint32_t v32; uint16_t v16;
    memcpy(h, "RIFF", 4); v32 = 36 + data; memcpy(h + 4, &v32, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v32 = 16; memcpy(h + 16, &v32, 4);
    v16 = 1; memcpy(h + 20, &v16, 2); v16 = 2; memcpy(h + 22, &v16, 2);
    v32 = 48000; memcpy(h + 24, &v32, 4); v32 = 192000; memcpy(h + 28, &v32, 4);
    v16 = 4; memcpy(h + 32, &v16, 2); v16 = 16; memcpy(h + 34, &v16, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(out, 0, SEEK_SET); fwrite(h, 1, 44, out);
}

#define CHECK(call) do { int rc = (call); printf("REC_%s_RC=%d\n", #call, rc); if (rc) { result = rc; goto cleanup; } } while (0)
int main(int argc, char **argv)
{
    if (argc < 3 || !freopen(argv[1], "w", stdout)) return 2;
    setbuf(stdout, 0);
    int source = argc > 3 ? atoi(argv[3]) : 0;
    int seconds = argc > 4 ? atoi(argv[4]) : 6;
    alarm(seconds + 10);
    out = fopen(argv[2], "wb");
    if (!out) return 3;
    header(0);
    OH_AudioStreamBuilder *b = 0; OH_AudioCapturer *c = 0; int result = 0;
    OH_AudioCapturer_Callbacks cb = {.OH_AudioCapturer_OnReadData = read_data};
    printf("REC_SOURCE=%d SECONDS=%d\n", source, seconds);
    CHECK(OH_AudioStreamBuilder_Create(&b, AUDIOSTREAM_TYPE_CAPTURER));
    CHECK(OH_AudioStreamBuilder_SetSamplingRate(b, 48000));
    CHECK(OH_AudioStreamBuilder_SetChannelCount(b, 2));
    CHECK(OH_AudioStreamBuilder_SetSampleFormat(b, AUDIOSTREAM_SAMPLE_S16LE));
    CHECK(OH_AudioStreamBuilder_SetCapturerInfo(b, (OH_AudioStream_SourceType)source));
    CHECK(OH_AudioStreamBuilder_SetCapturerCallback(b, cb, 0));
    CHECK(OH_AudioStreamBuilder_GenerateCapturer(b, &c));
    CHECK(OH_AudioCapturer_Start(c));
    sleep((unsigned)seconds);
    CHECK(OH_AudioCapturer_Stop(c));
    printf("REC_CAPTURED bytes=%llu peak=%d\n", (unsigned long long)bytes, peak);
cleanup:
    if (c) printf("REC_RELEASE_RC=%d\n", OH_AudioCapturer_Release(c));
    if (b) OH_AudioStreamBuilder_Destroy(b);
    header((uint32_t)bytes); fclose(out);
    printf("REC_EXIT=%d\n", result);
    return result ? 1 : 0;
}
