/* SPDX-License-Identifier: MIT
 * asteroids: msm_video's encoder on its own, without HarmonyOS, through v4l2enc.c: encodes <frames> frames and writes
 * the Annex-B stream. "v4l2_encode_test <h264|hevc> <out> [frames] [WxH] [nv12|nv21] [bands|noise|<file>] [bitrate]
 * [first timestamp, us] [paced]". The picture: coloured horizontal bands (the chroma's rows against the luma's show the
 * layout is right) and a white box moving across, 4 pixels a frame; faint noise (16 levels), which only rate control
 * keeps to a bitrate; or <file>, raw frames (tightly packed NV12 of that size), looped. paced: one frame every 33 ms, as
 * a camera gives them, instead of as fast as the encoder takes them. */
#include "v4l2enc.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *out;
static long packets, keyframes, configs, bytes, done_frames;
static int eos, failed;
static struct v4l2enc *enc;

static void on_input_done(void *u, uint64_t cookie) { (void)u; (void)cookie; done_frames++; }
static void on_packet(void *u, const struct v4l2enc_packet *p)
{
    (void)u;
    fwrite(p->data, 1, p->len, out);
    if (packets < 3 || p->keyframe || p->config)
        printf("packet %ld: %zu bytes ts %lld%s%s\n", packets, p->len, (long long)p->timestamp,
            p->keyframe ? " key" : "", p->config ? " config" : "");
    packets++; bytes += p->len; keyframes += p->keyframe; configs += p->config;
    v4l2enc_release_packet(enc, p->index);
}
static void on_eos(void *u) { (void)u; eos = 1; }
static void on_error(void *u, int err) { (void)u; printf("error %d\n", err); failed = 1; }
static void on_log(void *u, const char *msg) { (void)u; printf("v4l2enc: %s\n", msg); }

/* bands of 1/8 the height: luma steps, chroma red, green, blue, ... (YUV) */
static int noise;
static FILE *raw;
static size_t rawframes;

static void draw(uint8_t *p, unsigned w, unsigned h, unsigned stride, unsigned scanlines, int nv21, long n)
{
    if (raw) {
        uint8_t *line = malloc(w);
        if (fseek(raw, (long)((size_t)(n % rawframes) * w * h * 3 / 2), SEEK_SET)) { free(line); return; }
        for (unsigned y = 0; y < h + h / 2; y++) {
            uint8_t *row = p + (size_t)(y < h ? y : scanlines + y - h) * stride;
            if (fread(line, 1, w, raw) != w) break;
            if (y < h || !nv21) memcpy(row, line, w);
            else for (unsigned i = 0; i + 1 < w; i += 2) { row[i] = line[i + 1]; row[i + 1] = line[i]; }
        }
        free(line);
        return;
    }
    if (noise) {
        static uint32_t x = 1;
        for (unsigned y = 0; y < h + h / 2; y++) {
            uint8_t *row = p + (size_t)(y < h ? y : scanlines + y - h) * stride;
            for (unsigned i = 0; i < w; i++) { x = x * 1664525 + 1013904223; row[i] = 120 + (x >> 28); }
        }
        return;
    }
    static const uint8_t bands[8][3] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {210, 16, 146},
        {170, 166, 16}, {106, 202, 222}, {235, 128, 128}, {16, 128, 128}};
    unsigned bx = (unsigned)(n * 4) % (w - 64);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *c = bands[y * 8 / h];
        uint8_t *row = p + (size_t)y * stride;
        memset(row, c[0], w);
        if (y >= h / 2 - 32 && y < h / 2 + 32) memset(row + bx, 235, 64);
    }
    uint8_t *uv = p + (size_t)stride * scanlines;
    for (unsigned y = 0; y < h / 2; y++) {
        const uint8_t *c = bands[y * 2 * 8 / h];
        uint8_t *row = uv + (size_t)y * stride;
        for (unsigned x = 0; x < w; x += 2) { row[x] = c[nv21 ? 2 : 1]; row[x + 1] = c[nv21 ? 1 : 2]; }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: v4l2_encode_test <h264|hevc> <out> [frames] [WxH] [nv12|nv21] [bands|noise|<file>] "
            "[bitrate] [first timestamp, us] [paced]\n");
        return 2;
    }
    setbuf(stdout, 0);
    struct v4l2enc_config cfg = {.codec = strcmp(argv[1], "hevc") ? V4L2ENC_H264 : V4L2ENC_HEVC,
        .width = 1920, .height = 1080, .fps_q16 = 30 << 16, .bitrate = 10000000, .gop = 30};
    long total = argc > 3 ? atol(argv[3]) : 90;
    if (argc > 4) sscanf(argv[4], "%ux%u", &cfg.width, &cfg.height);
    cfg.nv21 = argc > 5 && !strcmp(argv[5], "nv21");
    noise = argc > 6 && !strcmp(argv[6], "noise");
    if (argc > 6 && !noise && strcmp(argv[6], "bands")) {
        raw = fopen(argv[6], "rb");
        if (!raw) { perror(argv[6]); return 3; }
        fseek(raw, 0, SEEK_END);
        rawframes = (size_t)ftell(raw) / ((size_t)cfg.width * cfg.height * 3 / 2);
        if (!rawframes) { fprintf(stderr, "%s: no whole frame\n", argv[6]); return 3; }
    }
    if (argc > 7) cfg.bitrate = atoi(argv[7]);
    int64_t ts0 = argc > 8 ? atoll(argv[8]) : 0;
    int paced = argc > 9 && !strcmp(argv[9], "paced");
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 3; }
    static const struct v4l2enc_ops ops = {on_input_done, on_packet, on_eos, on_error, on_log};
    enc = v4l2enc_open(&cfg, &ops, 0);
    if (!enc) return 4;
    unsigned stride, scanlines;
    v4l2enc_layout(enc, &stride, &scanlines);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long queued = 0;
    int draining = 0, idle = 0;
    while (!eos && !failed && idle < 2000) {
        while (queued < total) {
            uint8_t *data;
            int i = v4l2enc_get_input(enc, &data);
            if (i < 0) break;
            if (paced && queued) usleep(33333);
            draw(data, cfg.width, cfg.height, stride, scanlines, cfg.nv21, queued);
            int r = v4l2enc_queue_input(enc, i, ts0 + queued * 1000000 / 30, queued);
            if (r) { printf("queue: %d\n", r); failed = 1; break; }
            queued++;
        }
        if (queued == total && !draining) { printf("drain: %d\n", v4l2enc_drain(enc)); draining = 1; }
        struct pollfd p = {v4l2enc_fd(enc), POLLIN | POLLOUT | POLLPRI, 0};
        poll(&p, 1, 2);
        int n = v4l2enc_poll(enc);
        if (n < 0) { printf("poll: %d\n", n); break; }
        idle = n ? 0 : idle + 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("RESULT frames=%ld consumed=%ld packets=%ld keyframes=%ld configs=%ld bytes=%ld (%.1f Mbit/s at 30 fps) "
        "eos=%d ms=%.0f fps=%.1f\n", queued, done_frames, packets, keyframes, configs, bytes,
        queued ? bytes * 8.0 * 30 / queued / 1e6 : 0, eos, ms, ms > 0 ? queued * 1000.0 / ms : 0);
    v4l2enc_close(enc);
    fclose(out);
    return eos && packets ? 0 : 1;
}
