/* SPDX-License-Identifier: MIT
 * asteroids: video decoding through HarmonyOS's public AVCodec API, as an app would: lists the decoders
 * av_codec_service offers for H.264 and HEVC (hardware and software), then demuxes <file> and decodes its video
 * track in buffer mode, counting output frames: "native_video_decode_check <log> <file.mp4> [decoder name|-] [frame.nv12]"
 * (the last writes frame 30). Run as an init service (asteroids_video_check) or from a root shell. */
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avdemuxer.h>
#include <multimedia/player_framework/native_avsource.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 64
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static uint32_t inputs[SLOTS];
static OH_AVBuffer *buffers[SLOTS];
static int count, error, eos;
static long frames;
static const char *dump;

static void on_error(OH_AVCodec *c, int32_t code, void *u) { (void)c; (void)u; printf("VDEC_ERROR=%d\n", code); error = code ? code : -1; }
static void on_format(OH_AVCodec *c, OH_AVFormat *f, void *u)
{
    (void)c; (void)u;
    int w = 0, h = 0;
    OH_AVFormat_GetIntValue(f, OH_MD_KEY_WIDTH, &w);
    OH_AVFormat_GetIntValue(f, OH_MD_KEY_HEIGHT, &h);
    printf("VDEC_STREAM_CHANGED %dx%d\n", w, h);
}
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
    if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        pthread_mutex_lock(&lock);
        eos = 1;
        pthread_cond_signal(&ready);
        pthread_mutex_unlock(&lock);
    } else if (frames++ == 30 && dump) {
        OH_AVFormat *f = OH_VideoDecoder_GetOutputDescription(c);
        int w = 0, h = 0, stride = 0;
        OH_AVFormat_GetIntValue(f, OH_MD_KEY_WIDTH, &w);
        OH_AVFormat_GetIntValue(f, OH_MD_KEY_HEIGHT, &h);
        OH_AVFormat_GetIntValue(f, OH_MD_KEY_VIDEO_STRIDE, &stride);
        OH_AVFormat_Destroy(f);
        uint8_t *p = OH_AVBuffer_GetAddr(b);
        FILE *o = p && w && h ? fopen(dump, "wb") : 0;
        printf("VDEC_DUMP %dx%d stride=%d size=%d %s\n", w, h, stride, attr.size, o ? dump : "failed");
        if (o) {
            if (stride < w) stride = w;
            /* raw NV12, tightly packed; chroma after the allocator's stride x height luma (buffer_vdi.cpp) */
            for (int y = 0; y < h; y++) fwrite(p + (size_t)y * stride, 1, w, o);
            for (int y = 0; y < h / 2; y++) fwrite(p + (size_t)stride * h + (size_t)y * stride, 1, w, o);
            fclose(o);
        }
    }
    OH_VideoDecoder_FreeOutputBuffer(c, index);
}

static void list(const char *mime)
{
    OH_AVCapability *hw = OH_AVCodec_GetCapabilityByCategory(mime, false, HARDWARE);
    OH_AVCapability *sw = OH_AVCodec_GetCapabilityByCategory(mime, false, SOFTWARE);
    printf("VDEC_CAPS %s hardware=%s software=%s\n", mime, hw ? OH_AVCapability_GetName(hw) : "-",
        sw ? OH_AVCapability_GetName(sw) : "-");
}

int main(int argc, char **argv)
{
    if (argc < 3 || !freopen(argv[1], "w", stdout)) return 2;
    setbuf(stdout, 0);
    list(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    list(OH_AVCODEC_MIMETYPE_VIDEO_HEVC);
    int fd = open(argv[2], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { printf("VDEC_OPEN_FAILED %s\n", argv[2]); return 3; }
    OH_AVSource *source = OH_AVSource_CreateWithFD(fd, 0, st.st_size);
    if (!source) { puts("VDEC_SOURCE_FAILED"); return 4; }
    OH_AVFormat *sf = OH_AVSource_GetSourceFormat(source);
    int tracks = 0, track = -1, w = 0, h = 0;
    OH_AVFormat_GetIntValue(sf, OH_MD_KEY_TRACK_COUNT, &tracks);
    const char *mime = 0;
    for (int i = 0; i < tracks && track < 0; i++) {
        OH_AVFormat *tf = OH_AVSource_GetTrackFormat(source, i);
        int type = -1;
        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_TRACK_TYPE, &type);
        if (type == MEDIA_TYPE_VID) {
            track = i;
            const char *m = 0;
            OH_AVFormat_GetStringValue(tf, OH_MD_KEY_CODEC_MIME, &m);
            mime = m ? strdup(m) : 0;
            OH_AVFormat_GetIntValue(tf, OH_MD_KEY_WIDTH, &w);
            OH_AVFormat_GetIntValue(tf, OH_MD_KEY_HEIGHT, &h);
        }
    }
    printf("VDEC_TRACK index=%d mime=%s %dx%d tracks=%d\n", track, mime ? mime : "?", w, h, tracks);
    if (track < 0 || !mime) return 5;
    OH_AVDemuxer *demuxer = OH_AVDemuxer_CreateWithSource(source);
    if (!demuxer || OH_AVDemuxer_SelectTrackByID(demuxer, track)) { puts("VDEC_DEMUXER_FAILED"); return 6; }
    if (argc > 4) dump = argv[4];
    OH_AVCodec *codec = argc > 3 && strcmp(argv[3], "-") ? OH_VideoDecoder_CreateByName(argv[3])
                                                         : OH_VideoDecoder_CreateByMime(mime);
    if (!codec) { puts("VDEC_CREATE_FAILED"); return 7; }
    OH_AVFormat *cfg = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_WIDTH, w);
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_HEIGHT, h);
    OH_AVFormat_SetIntValue(cfg, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVCodecCallback cb = {on_error, on_format, on_input, on_output};
    int rc = OH_VideoDecoder_RegisterCallback(codec, cb, 0);
    if (!rc) rc = OH_VideoDecoder_Configure(codec, cfg);
    if (!rc) rc = OH_VideoDecoder_Prepare(codec);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!rc) rc = OH_VideoDecoder_Start(codec);
    printf("VDEC_START_RC=%d\n", rc);
    long samples = 0;
    int input_eos = 0;
    while (!rc && !error && !eos) {
        pthread_mutex_lock(&lock);
        while (!count && !error && !eos) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            if (pthread_cond_timedwait(&ready, &lock, &deadline)) break;
        }
        if (!count) { pthread_mutex_unlock(&lock); if (!eos) puts("VDEC_STALLED"); break; }
        uint32_t index = inputs[0];
        OH_AVBuffer *b = buffers[0];
        memmove(inputs, inputs + 1, --count * sizeof(*inputs));
        memmove(buffers, buffers + 1, count * sizeof(*buffers));
        pthread_mutex_unlock(&lock);
        if (input_eos) continue;
        OH_AVDemuxer_ReadSampleBuffer(demuxer, track, b);
        OH_AVCodecBufferAttr attr = {0};
        OH_AVBuffer_GetBufferAttr(b, &attr);
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) input_eos = 1; else samples++;
        OH_VideoDecoder_PushInputBuffer(codec, index);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("VDEC_RESULT samples=%ld frames=%ld eos=%d error=%d ms=%.0f fps=%.1f\n", samples, frames, eos, error, ms,
        ms > 0 ? frames * 1000.0 / ms : 0);
    OH_VideoDecoder_Stop(codec);
    OH_VideoDecoder_Destroy(codec);
    OH_AVDemuxer_Destroy(demuxer);
    OH_AVSource_Destroy(source);
    close(fd);
    return frames > 0 && eos ? 0 : 1;
}
