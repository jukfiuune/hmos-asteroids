/* SPDX-License-Identifier: MIT
 * asteroids: test client for sheng's PAL server, run as root beside HarmonyOS: opens a speaker stream on
 * @sheng.pal.pcm.v1 and writes 48 kHz stereo S16: "pal_tone [seconds] [amplitude]" (default 1 s, 3000; 0 = silence).
 * "pal_tone rec <seconds> <out.wav>" records from the handset mic instead (the server built with capture) and prints
 * the peak; "pal_tone play <file.wav>" plays such a recording (48 kHz stereo S16, header skipped). "pal_tone voice
 * on|off|speaker|earpiece" drives the modem voice stream (SUB1) as asteroids_atd does. Prints each request's result
 * and the server's frame count. */
#include "pal_client.h"
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int record(double seconds, const char *path)
{
    struct sheng_pal_reply reply = {0};
    int fd = sheng_pal_connect();
    if (fd < 0) { printf("PAL_REC_CONNECT=%d\n", fd); return 1; }
    printf("PAL_REC_STATUS=%d\n", sheng_pal_request(fd, SHENG_PAL_CAPTURE_STATUS, 0, 0, 0, 0, &reply));
    close(fd);
    fd = sheng_pal_connect();
    int rc = sheng_pal_request(fd, SHENG_PAL_CAPTURE_OPEN, 48000, 2, 0, 0, &reply);
    printf("PAL_REC_OPEN=%d\n", rc);
    if (rc) return 2;
    rc = sheng_pal_request(fd, SHENG_PAL_CAPTURE_START, 0, 0, 0, 0, &reply);
    printf("PAL_REC_START=%d\n", rc);
    FILE *out = fopen(path, "wb");
    if (!out) return 3;
    unsigned char header[44] = {0};
    fwrite(header, 1, 44, out);
    static short pcm[SHENG_PAL_CAPTURE_CHUNK / 2];
    unsigned long frames = 0, total = (unsigned long)(seconds * 48000);
    int peak = 0;
    while (!rc && frames < total) {
        rc = sheng_pal_capture_read(fd, pcm, SHENG_PAL_CAPTURE_CHUNK, &reply);
        if (rc) break;
        for (unsigned i = 0; i < reply.bytes / 2; i++) { int v = abs(pcm[i]); if (v > peak) peak = v; }
        fwrite(pcm, 1, reply.bytes, out);
        frames += reply.bytes / 4;
    }
    printf("PAL_REC_READ=%d frames=%lu peak=%d\n", rc, frames, peak);
    printf("PAL_REC_CLOSE=%d\n", sheng_pal_request(fd, SHENG_PAL_CAPTURE_CLOSE, 0, 0, 0, 0, &reply));
    uint32_t data = frames * 4, v32;
    uint16_t v16;
    memcpy(header, "RIFF", 4); v32 = 36 + data; memcpy(header + 4, &v32, 4);
    memcpy(header + 8, "WAVEfmt ", 8); v32 = 16; memcpy(header + 16, &v32, 4);
    v16 = 1; memcpy(header + 20, &v16, 2); v16 = 2; memcpy(header + 22, &v16, 2);
    v32 = 48000; memcpy(header + 24, &v32, 4); v32 = 192000; memcpy(header + 28, &v32, 4);
    v16 = 4; memcpy(header + 32, &v16, 2); v16 = 16; memcpy(header + 34, &v16, 2);
    memcpy(header + 36, "data", 4); memcpy(header + 40, &data, 4);
    fseek(out, 0, SEEK_SET); fwrite(header, 1, 44, out); fclose(out);
    return rc ? 4 : 0;
}

static int play(const char *path)
{
    FILE *in = fopen(path, "rb");
    if (!in) { perror(path); return 1; }
    fseek(in, 44, SEEK_SET);
    struct sheng_pal_reply reply = {0};
    int fd = sheng_pal_connect();
    if (fd < 0) { printf("PAL_PLAY_CONNECT=%d\n", fd); return 1; }
    int rc = sheng_pal_request(fd, SHENG_PAL_OPEN, 48000, 2, 0, 0, &reply);
    if (!rc) rc = sheng_pal_request(fd, SHENG_PAL_START, 0, 0, 0, 0, &reply);
    static short pcm[4800 * 2];
    size_t n;
    while (!rc && (n = fread(pcm, 4, 4800, in)) > 0)
        rc = sheng_pal_request(fd, SHENG_PAL_WRITE, 0, 0, pcm, n * 4, &reply);
    printf("PAL_PLAY=%d frames=%llu\n", rc, (unsigned long long)reply.written_frames);
    sheng_pal_request(fd, SHENG_PAL_STOP, 0, 0, 0, 0, &reply);
    sheng_pal_request(fd, SHENG_PAL_CLOSE, 0, 0, 0, 0, &reply);
    fclose(in);
    return rc ? 2 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "play")) return play(argv[2]);
    if (argc > 2 && !strcmp(argv[1], "voice")) {
        /* PAL device ids: earpiece 2, speaker 3, handset mic 27, speaker mic 28; SUB1's VSID. */
        struct sheng_pal_reply reply = {0};
        int fd = sheng_pal_connect(), rc;
        if (fd < 0) { printf("PAL_VOICE_CONNECT=%d\n", fd); return 1; }
        if (!strcmp(argv[2], "on")) rc = sheng_pal_request(fd, SHENG_PAL_VOICE_START, 2 | 27 << 16, 0x11C05000, 0, 0, &reply);
        else if (!strcmp(argv[2], "off")) rc = sheng_pal_request(fd, SHENG_PAL_VOICE_STOP, 0, 0, 0, 0, &reply);
        else if (!strcmp(argv[2], "speaker")) rc = sheng_pal_request(fd, SHENG_PAL_VOICE_ROUTE, 3 | 28 << 16, 0, 0, 0, &reply);
        else rc = sheng_pal_request(fd, SHENG_PAL_VOICE_ROUTE, 2 | 27 << 16, 0, 0, 0, &reply);
        printf("PAL_VOICE_%s=%d\n", argv[2], rc);
        return rc ? 1 : 0;
    }
    if (argc > 3 && !strcmp(argv[1], "rec")) return record(atof(argv[2]), argv[3]);
    double seconds = argc > 1 ? atof(argv[1]) : 1.0;
    int amplitude = argc > 2 ? atoi(argv[2]) : 3000;
    struct sheng_pal_reply reply = {0};
    int fd = sheng_pal_connect();
    if (fd < 0) { printf("PAL_TONE_CONNECT=%d\n", fd); return 1; }
    int rc = sheng_pal_request(fd, SHENG_PAL_OPEN, 48000, 2, 0, 0, &reply);
    printf("PAL_TONE_OPEN=%d\n", rc);
    if (rc) return 2;
    rc = sheng_pal_request(fd, SHENG_PAL_START, 0, 0, 0, 0, &reply);
    printf("PAL_TONE_START=%d\n", rc);
    static short pcm[480 * 2 * 10];    /* 100 ms */
    unsigned long frame = 0, total = (unsigned long)(seconds * 48000);
    while (!rc && frame < total) {
        for (int i = 0; i < 4800; i++, frame++)
            pcm[2 * i] = pcm[2 * i + 1] = (short)(amplitude * sin(6.283185307179586 * 440.0 * frame / 48000.0));
        rc = sheng_pal_request(fd, SHENG_PAL_WRITE, 0, 0, pcm, sizeof(pcm), &reply);
    }
    printf("PAL_TONE_WRITE=%d frames=%llu\n", rc, (unsigned long long)reply.written_frames);
    printf("PAL_TONE_STOP=%d\n", sheng_pal_request(fd, SHENG_PAL_STOP, 0, 0, 0, 0, &reply));
    printf("PAL_TONE_CLOSE=%d\n", sheng_pal_request(fd, SHENG_PAL_CLOSE, 0, 0, 0, 0, &reply));
    return rc ? 3 : 0;
}
