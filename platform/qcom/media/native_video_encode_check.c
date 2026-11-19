/* SPDX-License-Identifier: MIT
 * asteroids: video encoding through HarmonyOS's public AVCodec API, as an app would: lists the encoders
 * av_codec_service offers for H.264 and HEVC, then encodes <frames> synthetic NV12 frames (a moving gradient) in
 * buffer mode and writes the Annex-B stream: "native_video_encode_check <log> <avc|hevc> <out.264|out.265>
 * [frames] [WxH] [encoder name|-]". Check the output with ffprobe/ffmpeg on the laptop. */
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS 64
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static uint32_t inputs[SLOTS];
static OH_AVBuffer *buffers[SLOTS];
static int count, error, eos;
static long packets, keyframes, bytes;
static FILE *out;

static void on_error(OH_AVCodec *c, int32_t code, void *u) { (void)c; (void)u; printf("VENC_ERROR=%d\n", code); error = code ? code : -1; }
static void on_format(OH_AVCodec *c, OH_AVFormat *f, void *u) { (void)c; (void)u; (void)f; puts("VENC_STREAM_CHANGED"); }
static void on_input(OH_AVCodec *c, uint32_t index, OH_AVBuffer *b, void *u)
{
    (void)c; (void)u;
    pthread_mutex_lock(&lock);
    if (count < SLOTS) { inputs[count] = index; buffers[count] = b; count++; }
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
}
static void on_output(OH_AVCodec *c, uint32_t index, OH_AVBuffer *b, void *u)
{
    (void)u;
    OH_AVCodecBufferAttr attr = {0};
    OH_AVBuffer_GetBufferAttr(b, &attr);
    if (attr.size > 0) {
        fwrite(OH_AVBuffer_GetAddr(b) + attr.offset, 1, attr.size, out);
        if (packets < 3 || (attr.flags & (AVCODEC_BUFFER_FLAGS_CODEC_DATA | AVCODEC_BUFFER_FLAGS_SYNC_FRAME)))
            printf("VENC_PACKET %ld size=%d flags=0x%x pts=%lld\n", packets, attr.size, attr.flags, (long long)attr.pts);
        packets++;
        bytes += attr.size;
        if (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) keyframes++;
    }
    if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        pthread_mutex_lock(&lock);
        eos = 1;
        pthread_cond_signal(&ready);
        pthread_mutex_unlock(&lock);
    }
    OH_VideoEncoder_FreeOutputBuffer(c, index);
}

static void list(const char *mime)
{
    OH_AVCapability *hw = OH_AVCodec_GetCapabilityByCategory(mime, true, HARDWARE);
    OH_AVCapability *sw = OH_AVCodec_GetCapabilityByCategory(mime, true, SOFTWARE);
    printf("VENC_CAPS %s hardware=%s software=%s\n", mime, hw ? OH_AVCapability_GetName(hw) : "-",
        sw ? OH_AVCapability_GetName(sw) : "-");
}

/* a gradient that moves 4 pixels a frame, with a colour that changes over time */
static void draw(uint8_t *p, int w, int h, int stride, int slice, long n)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) p[(size_t)y * stride + x] = (uint8_t)((x + y + n * 4) & 0xff);
    uint8_t *uv = p + (size_t)stride * slice;
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w; x += 2) {
            uv[(size_t)y * stride + x] = (uint8_t)(128 + 60 * ((n / 30) % 2 ? 1 : -1));
            uv[(size_t)y * stride + x + 1] = (uint8_t)(x * 255 / w);
        }
}

int main(int argc, char **argv)
{
    if (argc < 4 || !freopen(argv[1], "w", stdout)) return 2;
    setbuf(stdout, 0);
    list(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    list(OH_AVCODEC_MIMETYPE_VIDEO_HEVC);
    const char *mime = strcmp(argv[2], "hevc") ? OH_AVCODEC_MIMETYPE_VIDEO_AVC : OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
    long total = argc > 4 ? atol(argv[4]) : 90;
    int w = 1920, h = 1080;
    if (argc > 5) sscanf(argv[5], "%dx%d", &w, &h);
    out = fopen(argv[3], "wb");
    if (!out) { printf("VENC_OPEN_FAILED %s\n", argv[3]); return 3; }
    OH_AVCodec *codec = argc > 6 && strcmp(argv[6], "-") ? OH_VideoEncoder_CreateByName(argv[6])
                                                         : OH_VideoEncoder_CreateByMime(mime);
    if (!codec) { puts("VENC_CREATE_FAILED"); return 7; }
    OH_AVFormat *cfg = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_WIDTH, w);
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_HEIGHT, h);
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetDoubleValue(cfg, OH_MD_KEY_FRAME_RATE, 30.0);
    OH_AVFormat_SetLongValue(cfg, OH_MD_KEY_BITRATE, 10000000);
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_I_FRAME_INTERVAL, 1000);
    OH_AVCodecCallback cb = {on_error, on_format, on_input, on_output};
    int rc = OH_VideoEncoder_RegisterCallback(codec, cb, 0);
    printf("VENC_REGISTER_RC=%d\n", rc);
    if (!rc) { rc = OH_VideoEncoder_Configure(codec, cfg); printf("VENC_CONFIGURE_RC=%d\n", rc); }
    if (!rc) { rc = OH_VideoEncoder_Prepare(codec); printf("VENC_PREPARE_RC=%d\n", rc); }
    int stride = w, slice = h;
    if (!rc) {
        OH_AVFormat *in = OH_VideoEncoder_GetInputDescription(codec);
        if (in) {
            OH_AVFormat_GetIntValue(in, OH_MD_KEY_VIDEO_STRIDE, &stride);
            OH_AVFormat_GetIntValue(in, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &slice);
            OH_AVFormat_Destroy(in);
        }
        if (stride < w) stride = w;
        if (slice < h) slice = h;
        printf("VENC_INPUT %dx%d stride=%d slice=%d\n", w, h, stride, slice);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!rc) rc = OH_VideoEncoder_Start(codec);
    printf("VENC_START_RC=%d\n", rc);
    long frames = 0;
    int input_eos = 0;
    while (!rc && !error && !eos) {
        pthread_mutex_lock(&lock);
        while (!count && !error && !eos) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            if (pthread_cond_timedwait(&ready, &lock, &deadline)) break;
        }
        if (!count) { pthread_mutex_unlock(&lock); if (!eos) puts("VENC_STALLED"); break; }
        uint32_t index = inputs[0];
        OH_AVBuffer *b = buffers[0];
        memmove(inputs, inputs + 1, --count * sizeof(*inputs));
        memmove(buffers, buffers + 1, count * sizeof(*buffers));
        pthread_mutex_unlock(&lock);
        if (input_eos) continue;
        OH_AVCodecBufferAttr attr = {0};
        if (frames < total) {
            uint8_t *p = OH_AVBuffer_GetAddr(b);
            int cap = OH_AVBuffer_GetCapacity(b);
            if (p && (size_t)cap >= (size_t)stride * slice * 3 / 2) draw(p, w, h, stride, slice, frames);
            else if (!frames) printf("VENC_INPUT_SMALL capacity=%d\n", cap);
            attr.size = stride * slice * 3 / 2;
            attr.pts = frames * 1000000 / 30;
            frames++;
        } else {
            attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
            input_eos = 1;
        }
        OH_AVBuffer_SetBufferAttr(b, &attr);
        OH_VideoEncoder_PushInputBuffer(codec, index);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("VENC_RESULT frames=%ld packets=%ld keyframes=%ld bytes=%ld eos=%d error=%d ms=%.0f fps=%.1f\n", frames,
        packets, keyframes, bytes, eos, error, ms, ms > 0 ? frames * 1000.0 / ms : 0);
    OH_VideoEncoder_Stop(codec);
    OH_VideoEncoder_Destroy(codec);
    fclose(out);
    return packets > 0 && eos ? 0 : 1;
}
