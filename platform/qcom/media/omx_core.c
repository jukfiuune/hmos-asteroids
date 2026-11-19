/* SPDX-License-Identifier: MIT
 * asteroids: libOMX_Core.z.so, the OpenMAX IL core the codec HDI loads (component_mgr.cpp, passthrough inside
 * av_codec_service), standing in for Huawei's. Its components are Qualcomm's msm_video decoder through v4l2dec.c,
 * under the names the donor's capability list (hdf_default.hcb) already offers: OMX.hisi.video.decoder.avc and .hevc;
 * and its encoder through v4l2enc.c as OMX.hisi.video.encoder.avc and .hevc (the list keeps those only while their
 * device, /dev/vcodec_venc, exists: the cfg links it to /dev/video33). Everything else the list names fails in
 * OMX_GetHandle, as Huawei's did here, and av_codec falls back to software.
 *
 * Decoding: bitstream comes in shared memory (CODEC_BUFFER_TYPE_AVSHARE_MEM_FD); decoded frames are copied from the
 * driver's buffers into HCodec's SurfaceBuffers (CODEC_BUFFER_TYPE_HANDLE, a BufferHandle from our allocator).
 * Encoding: frames come as SurfaceBuffers (CODEC_BUFFER_TYPE_DYNAMIC_HANDLE, from the encoder's input surface, which
 * the camera host or the app fills), are copied into the driver's buffers, and the bitstream goes out in shared
 * memory, the parameter sets first in a buffer of their own (CODECCONFIG) for the muxer. One worker thread per
 * component runs commands, feeds the codec and returns buffers; the client's threads only queue work for it. */
#include "v4l2dec.h"
#include "v4l2enc.h"
#include <OMX_Component.h>
#include <OMX_Core.h>
#include <OMX_Video.h>
#include <codec_omx_ext.h>
#include <dlfcn.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* graphic_surface's buffer_handle.h and the codec HDI's DynamicBuffer */
typedef struct {
    int32_t fd, width, stride, height, size, format;
    uint64_t usage;
    void *virAddr;
    uint64_t phyAddr;
    uint32_t reserveFds, reserveInts;
    int32_t reserve[0];
} BufferHandle;
struct DynamicBuffer { int32_t type; BufferHandle *bufferHandle; };

#define GRAPHIC_PIXEL_FMT_YCBCR_420_SP 24
#define GRAPHIC_PIXEL_FMT_YCRCB_420_SP 25
#define BUFFER_USAGE_CPU_READ (1ULL << 0)
#define BUFFER_USAGE_CPU_WRITE (1ULL << 1)
#define BUFFER_USAGE_MEM_DMA (1ULL << 3)

static void logf_(int level, const char *fmt, ...)
{
    static int (*print)(int, int, unsigned, const char *, const char *, ...);
    static int looked;
    if (!looked) {
        /* passthrough libraries live in their own linker namespace, where RTLD_DEFAULT may not reach libhilog */
        void *sym = dlsym(RTLD_DEFAULT, "HiLogPrint");
        const char *libs[] = {"libhilog.so", "libhilog_ndk.z.so"};
        for (unsigned i = 0; !sym && i < 2; i++) {
            void *lib = dlopen(libs[i], RTLD_NOW);
            if (lib) sym = dlsym(lib, "HiLogPrint");
        }
        print = (int (*)(int, int, unsigned, const char *, const char *, ...))sym;
        looked = 1;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (print) print(3 /* LOG_CORE */, level, 0xD002B33, "asteroids_omx", "%{public}s", msg);
}
#define LOGI(...) logf_(4, __VA_ARGS__)
#define LOGW(...) logf_(5, __VA_ARGS__)
#define LOGE(...) logf_(6, __VA_ARGS__)

struct kind { const char *name, *role, *mime; enum v4l2dec_codec codec; OMX_VIDEO_CODINGTYPE coding; int enc; };
static const struct kind kinds[] = {
    {"OMX.hisi.video.decoder.avc", "video_decoder.avc", "video/avc", V4L2DEC_H264, OMX_VIDEO_CodingAVC, 0},
    {"OMX.hisi.video.decoder.hevc", "video_decoder.hevc", "video/hevc", V4L2DEC_HEVC,
        (OMX_VIDEO_CODINGTYPE)CODEC_OMX_VIDEO_CodingHEVC, 0},
    {"OMX.hisi.video.encoder.avc", "video_encoder.avc", "video/avc", V4L2DEC_H264, OMX_VIDEO_CodingAVC, 1},
    {"OMX.hisi.video.encoder.hevc", "video_encoder.hevc", "video/hevc", V4L2DEC_HEVC,
        (OMX_VIDEO_CODINGTYPE)CODEC_OMX_VIDEO_CodingHEVC, 1},
};
#define NKINDS (sizeof(kinds) / sizeof(kinds[0]))

#define IN 0
#define OUTP 1
#define MAXBUF 64
#define MAXFRAMES 32
#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

struct fifo { OMX_BUFFERHEADERTYPE *b[MAXBUF]; unsigned head, n; };
static void push(struct fifo *f, OMX_BUFFERHEADERTYPE *b) { if (f->n < MAXBUF) f->b[(f->head + f->n++) % MAXBUF] = b; }
static OMX_BUFFERHEADERTYPE *pop(struct fifo *f)
{
    if (!f->n) return 0;
    OMX_BUFFERHEADERTYPE *b = f->b[f->head];
    f->head = (f->head + 1) % MAXBUF; f->n--;
    return b;
}

struct port {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    OMX_BUFFERHEADERTYPE *buf[MAXBUF];
    unsigned nbuf;
    uint32_t type;                       /* CODEC_BUFFER_TYPE_*, from OMX_IndexParamUseBufferType */
    struct fifo q;                       /* buffers the client gave us */
    int enabling, disabling;
};

/* what we keep per header in pPlatformPrivate */
struct bufpriv { int own, port; void *map; size_t maplen; int mapfd; };

struct frame { unsigned index; const uint8_t *data; unsigned stride, scanlines; int64_t ts; };
/* an encoded packet the driver gave us; off: how much of it has gone out (the parameter sets on their own first) */
struct packet { unsigned index; const uint8_t *data; size_t len, off; int64_t ts; int key, config; };
#define MAXPACKETS 16

struct cmd { OMX_COMMANDTYPE cmd; OMX_U32 param; };

struct comp {
    OMX_COMPONENTTYPE *h;
    const struct kind *kind;
    OMX_CALLBACKTYPE cb;
    OMX_PTR app;
    OMX_STATETYPE state, target;         /* target != state while a StateSet waits for buffers */
    struct port port[2];
    pthread_mutex_t lock;
    pthread_t thread;
    int wake, quit;
    struct cmd cmds[32];
    unsigned ncmd;
    struct v4l2dec *dec;
    struct v4l2enc *enc;
    struct packet packets[MAXPACKETS];
    unsigned npackets;
    int config_sent;                     /* the encoder's parameter sets went out */
    unsigned bitrate, gop, cbr;          /* encoder settings from the client */
    /* the driver's timeline: microseconds from the first frame (see driver_ts), and each frame's own timestamp by it */
    int64_t drv_ts, last_in_ts;
    struct { int64_t drv, in; } ts_map[MAXBUF];
    unsigned ts_next;
    struct frame frames[MAXFRAMES];
    unsigned nframes;
    unsigned vis_w, vis_h;               /* the decoder's visible size */
    int draining, eos_out;               /* input EOS seen; the EOS output buffer is owed */
    OMX_U32 eos_flags;
    int error, nv21;
    /* per stream, logged at end of stream: frames, the copy, and time frames waited for an output buffer */
    unsigned stat_frames;
    uint64_t stat_bytes;                 /* encoder: bitstream out */
    int64_t stat_start, stat_copy, stat_wait, stat_ready[MAXFRAMES];
};

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void kick(struct comp *c) { uint64_t one = 1; if (write(c->wake, &one, 8) < 0) {} }

static void init_header(OMX_BUFFERHEADERTYPE *b) { b->nSize = sizeof(*b); b->nVersion.nVersion = OMX_VERSION; }
#define INIT(p) do { memset(&(p), 0, sizeof(p)); (p).nSize = sizeof(p); (p).nVersion.nVersion = OMX_VERSION; } while (0)

static void set_output_geometry(struct comp *c, unsigned w, unsigned h)
{
    OMX_VIDEO_PORTDEFINITIONTYPE *v = &c->port[OUTP].def.format.video;
    v->nFrameWidth = w; v->nFrameHeight = h;
    v->nStride = ALIGN(w, 16); v->nSliceHeight = ALIGN(h, 16);
    c->port[OUTP].def.nBufferSize = v->nStride * v->nSliceHeight * 3 / 2;
}

/* The encoder's ports: raw frames in (as our allocator lays them out), bitstream out. */
static void set_encoder_geometry(struct comp *c, unsigned w, unsigned h)
{
    OMX_VIDEO_PORTDEFINITIONTYPE *in = &c->port[IN].def.format.video, *out = &c->port[OUTP].def.format.video;
    in->nFrameWidth = out->nFrameWidth = w;
    in->nFrameHeight = out->nFrameHeight = h;
    in->nStride = out->nStride = w;
    in->nSliceHeight = out->nSliceHeight = h;
    c->port[IN].def.nBufferSize = ALIGN(w, 16) * ALIGN(h, 16) * 3 / 2;
    /* a key frame at a high bitrate fits, as the driver's own bitstream buffers do */
    c->port[OUTP].def.nBufferSize = w * h < (1 << 20) ? 1 << 20 : w * h;
}

static void init_encoder_ports(struct comp *c)
{
    for (int i = 0; i < 2; i++) {
        OMX_PARAM_PORTDEFINITIONTYPE *d = &c->port[i].def;
        INIT(*d);
        d->nPortIndex = i;
        d->eDir = i == IN ? OMX_DirInput : OMX_DirOutput;
        d->nBufferCountMin = 4;
        d->nBufferCountActual = 8;
        d->bEnabled = OMX_TRUE;
        d->eDomain = OMX_PortDomainVideo;
        d->nBufferAlignment = 1;
        d->format.video.xFramerate = 30 << 16;
    }
    OMX_VIDEO_PORTDEFINITIONTYPE *in = &c->port[IN].def.format.video, *out = &c->port[OUTP].def.format.video;
    in->cMIMEType = (OMX_STRING)"video/raw";
    in->eCompressionFormat = OMX_VIDEO_CodingUnused;
    in->eColorFormat = (OMX_COLOR_FORMATTYPE)GRAPHIC_PIXEL_FMT_YCBCR_420_SP;
    out->cMIMEType = (OMX_STRING)c->kind->mime;
    out->eCompressionFormat = c->kind->coding;
    out->eColorFormat = OMX_COLOR_FormatUnused;
    out->nBitrate = c->bitrate = 10000000;
    set_encoder_geometry(c, 1920, 1080);
}

static void init_ports(struct comp *c)
{
    if (c->kind->enc) { init_encoder_ports(c); return; }
    for (int i = 0; i < 2; i++) {
        OMX_PARAM_PORTDEFINITIONTYPE *d = &c->port[i].def;
        INIT(*d);
        d->nPortIndex = i;
        d->eDir = i == IN ? OMX_DirInput : OMX_DirOutput;
        d->nBufferCountActual = d->nBufferCountMin = i == IN ? 4 : 6;
        d->bEnabled = OMX_TRUE;
        d->eDomain = OMX_PortDomainVideo;
        d->nBufferAlignment = 1;
        d->format.video.xFramerate = 60 << 16;
    }
    OMX_VIDEO_PORTDEFINITIONTYPE *in = &c->port[IN].def.format.video;
    in->cMIMEType = (OMX_STRING)c->kind->mime;
    in->nFrameWidth = 1920; in->nFrameHeight = 1080; in->nStride = 1920; in->nSliceHeight = 1080;
    in->eCompressionFormat = c->kind->coding;
    in->eColorFormat = OMX_COLOR_FormatUnused;
    c->port[IN].def.nBufferSize = 2 << 20;
    OMX_VIDEO_PORTDEFINITIONTYPE *out = &c->port[OUTP].def.format.video;
    out->cMIMEType = (OMX_STRING)"video/raw";
    out->eCompressionFormat = OMX_VIDEO_CodingUnused;
    /* HCodec reads the output's eColorFormat as a GraphicPixelFormat, not an OMX_COLOR_FORMATTYPE */
    out->eColorFormat = (OMX_COLOR_FORMATTYPE)GRAPHIC_PIXEL_FMT_YCBCR_420_SP;
    set_output_geometry(c, 1920, 1080);
}

/* ---- callbacks to the client, made from the worker without our lock ---- */

static void emit_event(struct comp *c, OMX_EVENTTYPE e, OMX_U32 d1, OMX_U32 d2)
{
    pthread_mutex_unlock(&c->lock);
    if (c->cb.EventHandler) c->cb.EventHandler(c->h, c->app, e, d1, d2, 0);
    pthread_mutex_lock(&c->lock);
}

static void return_buffer(struct comp *c, OMX_BUFFERHEADERTYPE *b)
{
    pthread_mutex_unlock(&c->lock);
    if (((struct bufpriv *)b->pPlatformPrivate)->port == OUTP) { if (c->cb.FillBufferDone) c->cb.FillBufferDone(c->h, c->app, b); }
    else if (c->cb.EmptyBufferDone) c->cb.EmptyBufferDone(c->h, c->app, b);
    pthread_mutex_lock(&c->lock);
}

static void return_all(struct comp *c, int port)
{
    OMX_BUFFERHEADERTYPE *b;
    while ((b = pop(&c->port[port].q))) {
        if (port == OUTP) { b->nFilledLen = 0; b->nFlags = 0; }
        return_buffer(c, b);
    }
}

/* ---- v4l2dec callbacks (worker thread, lock held) ---- */

static void on_input_done(void *u, uint64_t cookie) { (void)u; (void)cookie; }

static void on_format(void *u, unsigned w, unsigned h, unsigned stride, unsigned scanlines)
{
    struct comp *c = u;
    LOGI("%s: decoder output %ux%u stride %u scanlines %u", c->kind->name, w, h, stride, scanlines);
    c->vis_w = w; c->vis_h = h;
}

static void on_frame(void *u, const struct v4l2dec_frame *f)
{
    struct comp *c = u;
    if (c->nframes == MAXFRAMES) { v4l2dec_release_frame(c->dec, f->index); return; }
    c->stat_ready[c->nframes] = now_us();
    c->frames[c->nframes++] = (struct frame){f->index, f->data, f->stride, f->scanlines, f->timestamp};
}

static void on_eos(void *u) { struct comp *c = u; c->eos_out = 1; }
static void on_error(void *u, int err) { struct comp *c = u; LOGE("%s: decoder error %d", c->kind->name, err); c->error = err; }

static const struct v4l2dec_ops dec_ops = {on_input_done, on_format, on_frame, on_eos, on_error};

/* ---- output copies ---- */

static uint8_t *map_handle(struct bufpriv *p, BufferHandle *h)
{
    if (h->virAddr) return h->virAddr;
    if (p->map && p->mapfd == h->fd && p->maplen == (size_t)h->size) return p->map;
    if (p->map) munmap(p->map, p->maplen);
    p->map = mmap(0, h->size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    if (p->map == MAP_FAILED) { p->map = 0; return 0; }
    p->maplen = h->size; p->mapfd = h->fd;
    return p->map;
}

static void copy_nv12(uint8_t *dst, unsigned dstride, unsigned dslice, size_t cap, const struct frame *f,
    unsigned w, unsigned h, int nv21)
{
    if ((size_t)dstride * dslice * 3 / 2 > cap || dstride < w || dslice < h) return;
    for (unsigned y = 0; y < h; y++) memcpy(dst + (size_t)y * dstride, f->data + (size_t)y * f->stride, w);
    const uint8_t *suv = f->data + (size_t)f->stride * f->scanlines;
    uint8_t *duv = dst + (size_t)dstride * dslice;
    for (unsigned y = 0; y < h / 2; y++) {
        const uint8_t *s = suv + (size_t)y * f->stride;
        uint8_t *d = duv + (size_t)y * dstride;
        if (!nv21) { memcpy(d, s, w); continue; }
        for (unsigned x = 0; x + 1 < w; x += 2) { d[x] = s[x + 1]; d[x + 1] = s[x]; }
    }
}

static int fill(struct comp *c, OMX_BUFFERHEADERTYPE *b, const struct frame *f)
{
    struct port *p = &c->port[OUTP];
    OMX_VIDEO_PORTDEFINITIONTYPE *v = &p->def.format.video;
    unsigned w = c->vis_w, h = c->vis_h;
    b->nOffset = 0;
    if (p->type == CODEC_BUFFER_TYPE_HANDLE || p->type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE) {
        BufferHandle *bh = p->type == CODEC_BUFFER_TYPE_HANDLE ? (BufferHandle *)b->pBuffer
                                                              : ((struct DynamicBuffer *)b->pBuffer)->bufferHandle;
        if (!bh) return -EINVAL;
        uint8_t *dst = map_handle(b->pPlatformPrivate, bh);
        if (!dst) return -errno;
        /* our allocator (platform/qcom/display/buffer_vdi.cpp): chroma right after stride x height luma rows */
        struct dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
        ioctl(bh->fd, DMA_BUF_IOCTL_SYNC, &s);
        copy_nv12(dst, bh->stride, bh->height, bh->size, f, w, h, bh->format == GRAPHIC_PIXEL_FMT_YCRCB_420_SP);
        s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        ioctl(bh->fd, DMA_BUF_IOCTL_SYNC, &s);
        b->nFilledLen = (OMX_U32)bh->stride * bh->height * 3 / 2;
    } else {
        copy_nv12(b->pBuffer, v->nStride, v->nSliceHeight, b->nAllocLen, f, w, h, c->nv21);
        b->nFilledLen = v->nStride * v->nSliceHeight * 3 / 2;
    }
    b->nTimeStamp = f->ts;
    b->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
    return 0;
}

/* ---- the worker ---- */

static int populated(struct port *p) { return !p->def.bEnabled || p->nbuf >= p->def.nBufferCountActual; }
static int empty(struct port *p) { return p->nbuf == 0; }

static void stop_decoder(struct comp *c)
{
    for (unsigned i = 0; i < c->nframes; i++) v4l2dec_release_frame(c->dec, c->frames[i].index);
    c->nframes = 0;
    v4l2dec_close(c->dec);
    c->dec = 0;
    c->draining = c->eos_out = 0;
}

static void stop_encoder(struct comp *c);

static void flush_port(struct comp *c, int port)
{
    if (c->enc) stop_encoder(c);  /* the next frame opens a new one */
    if (c->dec && port == IN) {
        for (unsigned i = 0; i < c->nframes; i++) v4l2dec_release_frame(c->dec, c->frames[i].index);
        c->nframes = 0;
        v4l2dec_flush(c->dec);
        c->draining = c->eos_out = 0;
    }
    if (c->dec && port == OUTP) {
        for (unsigned i = 0; i < c->nframes; i++) v4l2dec_release_frame(c->dec, c->frames[i].index);
        c->nframes = 0;
    }
    return_all(c, port);
}

/* Finish whatever pending state change or port enable/disable the buffers now allow. */
static void settle(struct comp *c)
{
    if (c->target != c->state) {
        int done = 0;
        if (c->state == OMX_StateLoaded && c->target == OMX_StateIdle)
            done = populated(&c->port[IN]) && populated(&c->port[OUTP]);
        else if (c->state == OMX_StateIdle && c->target == OMX_StateLoaded)
            done = empty(&c->port[IN]) && empty(&c->port[OUTP]);
        else done = 1;
        if (done) {
            c->state = c->target;
            LOGI("%s: state %d", c->kind->name, c->state);
            emit_event(c, OMX_EventCmdComplete, OMX_CommandStateSet, c->state);
        }
    }
    for (int i = 0; i < 2; i++) {
        struct port *p = &c->port[i];
        if (p->disabling && empty(p)) {
            p->disabling = 0;
            emit_event(c, OMX_EventCmdComplete, OMX_CommandPortDisable, i);
        }
        if (p->enabling && (c->state == OMX_StateLoaded || p->nbuf >= p->def.nBufferCountActual)) {
            p->enabling = 0;
            emit_event(c, OMX_EventCmdComplete, OMX_CommandPortEnable, i);
        }
    }
}

static void run_cmd(struct comp *c, struct cmd k)
{
    switch (k.cmd) {
    case OMX_CommandStateSet: {
        OMX_STATETYPE to = (OMX_STATETYPE)k.param;
        LOGI("%s: state %d -> %d", c->kind->name, c->state, to);
        if (to == c->state) { emit_event(c, OMX_EventError, OMX_ErrorSameState, 0); break; }
        if ((c->state == OMX_StateExecuting || c->state == OMX_StatePause) && to == OMX_StateIdle) {
            if (c->dec) stop_decoder(c);
            if (c->enc) stop_encoder(c);
            return_all(c, IN);
            return_all(c, OUTP);
        }
        if (to == OMX_StateExecuting && !c->dec && !c->kind->enc) {
            OMX_VIDEO_PORTDEFINITIONTYPE *v = &c->port[IN].def.format.video;
            c->dec = v4l2dec_open(c->kind->codec, v->nFrameWidth, v->nFrameHeight, &dec_ops, c);
            if (!c->dec) {
                LOGE("%s: v4l2dec_open failed", c->kind->name);
                emit_event(c, OMX_EventError, OMX_ErrorInsufficientResources, 0);
                break;
            }
            c->vis_w = c->port[OUTP].def.format.video.nFrameWidth;
            c->vis_h = c->port[OUTP].def.format.video.nFrameHeight;
        }
        c->target = to;
        break;
    }
    case OMX_CommandFlush:
        for (int i = 0; i < 2; i++)
            if (k.param == (OMX_U32)i || k.param == OMX_ALL) {
                flush_port(c, i);
                emit_event(c, OMX_EventCmdComplete, OMX_CommandFlush, i);
            }
        break;
    case OMX_CommandPortDisable:
        /* decoded frames wait in c->frames (and the driver) for the new buffers */
        for (int i = 0; i < 2; i++)
            if (k.param == (OMX_U32)i || k.param == OMX_ALL) {
                if (i == IN) flush_port(c, i); else return_all(c, i);
                c->port[i].def.bEnabled = OMX_FALSE;
                c->port[i].disabling = 1;
            }
        break;
    case OMX_CommandPortEnable:
        for (int i = 0; i < 2; i++)
            if (k.param == (OMX_U32)i || k.param == OMX_ALL) {
                c->port[i].def.bEnabled = OMX_TRUE;
                c->port[i].enabling = 1;
            }
        break;
    default:
        emit_event(c, OMX_EventError, OMX_ErrorUnsupportedIndex, k.cmd);
    }
}

/* Whether Annex-B data holds a picture (a VCL NAL unit), not only parameter sets: av_codec's HEVC demuxer flags the
 * first sample CODECCONFIG with the IDR picture after its VPS/SPS/PPS. */
static int has_picture(const struct comp *c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        uint8_t h = p[i + 3];
        if (c->kind->codec == V4L2DEC_H264 ? (h & 0x1f) >= 1 && (h & 0x1f) <= 5 : ((h >> 1) & 0x3f) < 32) return 1;
        i += 2;
    }
    return 0;
}

static int running(struct comp *c, int port) { return c->state == OMX_StateExecuting && c->port[port].def.bEnabled && !c->port[port].disabling; }

static void feed(struct comp *c)
{
    struct port *in = &c->port[IN];
    while (c->dec && running(c, IN) && in->q.n && !c->draining) {
        OMX_BUFFERHEADERTYPE *b = in->q.b[in->q.head];
        const uint8_t *data = b->pBuffer + b->nOffset;
        int r = 0;
        if (b->nFilledLen && (b->nFlags & OMX_BUFFERFLAG_CODECCONFIG) && !has_picture(c, data, b->nFilledLen))
            r = v4l2dec_queue_config(c->dec, data, b->nFilledLen);
        else if (b->nFilledLen)
            r = v4l2dec_queue(c->dec, data, b->nFilledLen, b->nTimeStamp, 0);
        if (!r && !c->stat_start) c->stat_start = now_us();
        if (r == -EAGAIN) break;
        if (r) LOGW("%s: queue %u bytes flags 0x%x: %d", c->kind->name, b->nFilledLen, b->nFlags, r);
        if (b->nFlags & OMX_BUFFERFLAG_EOS) {
            c->draining = 1;
            c->eos_flags = b->nFlags;
            if (v4l2dec_drain(c->dec)) c->eos_out = 1;  /* nothing to drain: the EOS buffer goes out at once */
        }
        pop(&in->q);
        b->nFilledLen = 0;
        return_buffer(c, b);
    }
}

static void deliver(struct comp *c)
{
    struct port *out = &c->port[OUTP];
    if (!running(c, OUTP)) return;
    /* a new size: the client reconfigures the output port (disable, new buffers, enable) before more frames */
    OMX_VIDEO_PORTDEFINITIONTYPE *v = &out->def.format.video;
    if (c->vis_w && (c->vis_w != v->nFrameWidth || c->vis_h != v->nFrameHeight)) {
        LOGI("%s: output port %ux%u -> %ux%u", c->kind->name, v->nFrameWidth, v->nFrameHeight, c->vis_w, c->vis_h);
        set_output_geometry(c, c->vis_w, c->vis_h);
        emit_event(c, OMX_EventPortSettingsChanged, OUTP, OMX_IndexParamPortDefinition);
        return;
    }
    while (c->nframes && out->q.n) {
        OMX_BUFFERHEADERTYPE *b = pop(&out->q);
        struct frame f = c->frames[0];
        int64_t t0 = now_us();
        c->stat_wait += t0 - c->stat_ready[0];
        memmove(c->frames, c->frames + 1, --c->nframes * sizeof(*c->frames));
        memmove(c->stat_ready, c->stat_ready + 1, c->nframes * sizeof(*c->stat_ready));
        if (fill(c, b, &f)) LOGW("%s: output copy failed", c->kind->name);
        c->stat_copy += now_us() - t0;
        c->stat_frames++;
        v4l2dec_release_frame(c->dec, f.index);
        return_buffer(c, b);
        if (!running(c, OUTP)) return;
    }
    if (c->eos_out && !c->nframes && out->q.n) {
        OMX_BUFFERHEADERTYPE *b = pop(&out->q);
        b->nFilledLen = 0; b->nOffset = 0;
        b->nFlags = OMX_BUFFERFLAG_EOS;
        c->eos_out = 0; c->draining = 0;
        int64_t ms = (now_us() - c->stat_start) / 1000;
        LOGI("%s: end of stream: %u frames in %lld ms, copy %lld us/frame, waited for output %lld us/frame",
            c->kind->name, c->stat_frames, (long long)ms, (long long)(c->stat_frames ? c->stat_copy / c->stat_frames : 0),
            (long long)(c->stat_frames ? c->stat_wait / c->stat_frames : 0));
        c->stat_frames = 0; c->stat_copy = c->stat_wait = c->stat_start = 0;
        return_buffer(c, b);
        emit_event(c, OMX_EventBufferFlag, OUTP, OMX_BUFFERFLAG_EOS);
    }
}

/* ---- the encoder (v4l2enc callbacks run on the worker thread with the lock held) ---- */

static void enc_input_done(void *u, uint64_t cookie) { (void)u; (void)cookie; }

/* The timestamp a frame came with, for a packet the driver stamped with driver_ts's. */
static int64_t client_ts(struct comp *c, int64_t drv)
{
    for (unsigned i = 0; i < MAXBUF; i++)
        if (c->ts_map[i].drv == drv) return c->ts_map[i].in;
    return drv;
}

static void enc_packet(void *u, const struct v4l2enc_packet *p)
{
    struct comp *c = u;
    if (c->npackets == MAXPACKETS) { v4l2enc_release_packet(c->enc, p->index); return; }
    c->packets[c->npackets++] = (struct packet){p->index, p->data, p->len, 0, client_ts(c, p->timestamp), p->keyframe,
        p->config};
}

static void enc_eos(void *u) { struct comp *c = u; c->eos_out = 1; }
static void enc_error(void *u, int err) { struct comp *c = u; LOGE("%s: encoder error %d", c->kind->name, err); c->error = err; }
static void enc_log(void *u, const char *msg) { LOGI("%s: %s", ((struct comp *)u)->kind->name, msg); }

static const struct v4l2enc_ops enc_ops = {enc_input_done, enc_packet, enc_eos, enc_error, enc_log};

static int open_encoder(struct comp *c)
{
    OMX_VIDEO_PORTDEFINITIONTYPE *v = &c->port[IN].def.format.video;
    struct v4l2enc_config cfg = {
        .codec = c->kind->codec == V4L2DEC_HEVC ? V4L2ENC_HEVC : V4L2ENC_H264,
        .width = v->nFrameWidth, .height = v->nFrameHeight, .nv21 = c->nv21,
        .fps_q16 = v->xFramerate ? v->xFramerate : 30 << 16, .bitrate = c->bitrate, .gop = c->gop,
    };
    LOGI("%s: open %ux%u %s %.2f fps %u bit/s gop %u", c->kind->name, cfg.width, cfg.height, c->nv21 ? "NV21" : "NV12",
        cfg.fps_q16 / 65536.0, cfg.bitrate, cfg.gop);
    c->enc = v4l2enc_open(&cfg, &enc_ops, c);
    c->config_sent = 0;
    c->ts_next = 0;
    memset(c->ts_map, 0xff, sizeof(c->ts_map));
    c->stat_frames = 0; c->stat_copy = 0; c->stat_start = 0; c->stat_bytes = 0;
    return c->enc ? 0 : -1;
}

static void log_encoder_stats(struct comp *c, const char *why)
{
    int64_t span = c->drv_ts;
    LOGI("%s: %s: %u frames over %lld ms of timestamps, %llu bytes (%.1f Mbit/s), copy %lld us/frame", c->kind->name,
        why, c->stat_frames, (long long)(span / 1000), (unsigned long long)c->stat_bytes,
        span > 0 ? c->stat_bytes * 8.0 / span : 0, (long long)(c->stat_frames ? c->stat_copy / c->stat_frames : 0));
}

static void stop_encoder(struct comp *c)
{
    if (c->stat_frames) log_encoder_stats(c, "stopped");
    for (unsigned i = 0; i < c->npackets; i++) v4l2enc_release_packet(c->enc, c->packets[i].index);
    c->npackets = 0;
    v4l2enc_close(c->enc);
    c->enc = 0;
    c->draining = c->eos_out = 0;
}

/* The timestamp the driver gets for a frame: its rate control spaces bits by it (msm_vidc took frames 33 s apart for
 * 0.03 fps and made 120 Mbit/s of a 17 Mbit/s recording). OMX timestamps are microseconds, and HCodec's buffer mode
 * gives those, but its surface mode (the camera's recordings) passes the surface's nanoseconds through. So the driver
 * gets a microsecond timeline of its own from the gaps between frames, a gap of a second or more counting as
 * nanoseconds, and packets go out with the frame's own timestamp. */
static int64_t driver_ts(struct comp *c, int64_t ts)
{
    if (c->stat_frames) {
        int64_t gap = ts - c->last_in_ts;
        if (gap >= 1000000) gap /= 1000;
        c->drv_ts += gap > 0 ? gap : 1;
    } else {
        c->drv_ts = 0;
    }
    c->last_in_ts = ts;
    c->ts_map[c->ts_next].drv = c->drv_ts;
    c->ts_map[c->ts_next].in = ts;
    c->ts_next = (c->ts_next + 1) % MAXBUF;
    return c->drv_ts;
}

/* One frame from the client's buffer into the encoder's, in the driver's layout (stride, chroma at scanlines). */
static int copy_frame(struct comp *c, OMX_BUFFERHEADERTYPE *b, uint8_t *dst)
{
    struct port *p = &c->port[IN];
    OMX_VIDEO_PORTDEFINITIONTYPE *v = &p->def.format.video;
    unsigned w = v->nFrameWidth, h = v->nFrameHeight, dstride, dslice;
    v4l2enc_layout(c->enc, &dstride, &dslice);
    const uint8_t *src;
    size_t sstride, uvoff, cap;
    int swap = 0;
    BufferHandle *bh = 0;
    if (p->type == CODEC_BUFFER_TYPE_HANDLE || p->type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE) {
        bh = p->type == CODEC_BUFFER_TYPE_HANDLE ? (BufferHandle *)b->pBuffer
                                                 : ((struct DynamicBuffer *)b->pBuffer)->bufferHandle;
        if (!bh) return -EINVAL;
        src = map_handle(b->pPlatformPrivate, bh);
        if (!src) return -errno;
        /* our allocator (platform/qcom/display/buffer_vdi.cpp): chroma right after stride x height luma rows */
        sstride = bh->stride; uvoff = (size_t)bh->stride * bh->height; cap = bh->size;
        swap = (bh->format == GRAPHIC_PIXEL_FMT_YCRCB_420_SP) != c->nv21;
        struct dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        ioctl(bh->fd, DMA_BUF_IOCTL_SYNC, &s);
    } else {
        src = b->pBuffer + b->nOffset;
        sstride = v->nStride; uvoff = sstride * v->nSliceHeight; cap = b->nAllocLen - b->nOffset;
    }
    int r = 0;
    if (sstride < w || uvoff + sstride * (h / 2) > cap) r = -EINVAL;
    else {
        for (unsigned y = 0; y < h; y++) memcpy(dst + (size_t)y * dstride, src + (size_t)y * sstride, w);
        for (unsigned y = 0; y < h / 2; y++) {
            const uint8_t *s = src + uvoff + (size_t)y * sstride;
            uint8_t *d = dst + (size_t)dstride * dslice + (size_t)y * dstride;
            if (!swap) { memcpy(d, s, w); continue; }
            for (unsigned x = 0; x + 1 < w; x += 2) { d[x] = s[x + 1]; d[x + 1] = s[x]; }
        }
    }
    if (bh) {
        struct dma_buf_sync s = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
        ioctl(bh->fd, DMA_BUF_IOCTL_SYNC, &s);
    }
    return r;
}

static int has_frame(struct comp *c, OMX_BUFFERHEADERTYPE *b)
{
    struct port *p = &c->port[IN];
    if (p->type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE) return b->pBuffer && ((struct DynamicBuffer *)b->pBuffer)->bufferHandle;
    if (p->type == CODEC_BUFFER_TYPE_HANDLE) return b->pBuffer != 0;
    return b->nFilledLen != 0;
}

static void enc_feed(struct comp *c)
{
    struct port *in = &c->port[IN];
    while (running(c, IN) && in->q.n && !c->draining) {
        OMX_BUFFERHEADERTYPE *b = in->q.b[in->q.head];
        if (!c->enc && open_encoder(c)) {
            LOGE("%s: v4l2enc_open failed", c->kind->name);
            emit_event(c, OMX_EventError, OMX_ErrorHardware, 0);
            return;
        }
        /* an end of stream comes without a frame: in surface mode without a handle, in buffer mode empty */
        if (has_frame(c, b) && !(b->nFlags & OMX_BUFFERFLAG_EOS && !b->nFilledLen)) {
            uint8_t *dst;
            int i = v4l2enc_get_input(c->enc, &dst);
            if (i == -EAGAIN) break;
            int64_t t0 = now_us();
            int r = copy_frame(c, b, dst);
            c->stat_copy += now_us() - t0;
            if (!r) r = v4l2enc_queue_input(c->enc, i, driver_ts(c, b->nTimeStamp), 0);
            if (r) LOGW("%s: frame %u flags 0x%x: %d", c->kind->name, c->stat_frames, b->nFlags, r);
            else {
                if (c->stat_frames < 2)
                    LOGI("%s: frame %u: timestamp %lld", c->kind->name, c->stat_frames, (long long)b->nTimeStamp);
                if (!c->stat_frames++) c->stat_start = now_us();
            }
        }
        if (b->nFlags & OMX_BUFFERFLAG_EOS) {
            c->draining = 1;
            c->eos_flags = b->nFlags;
            if (!c->stat_frames || v4l2enc_drain(c->enc)) c->eos_out = 1;
        }
        pop(&in->q);
        b->nFilledLen = 0;
        return_buffer(c, b);
    }
}

/* Where the first picture starts in Annex-B data (its NAL unit's start code), or n if there is none. */
static size_t first_picture(const struct comp *c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        uint8_t h = p[i + 3];
        if (c->kind->codec == V4L2DEC_H264 ? (h & 0x1f) >= 1 && (h & 0x1f) <= 5 : ((h >> 1) & 0x3f) < 32)
            return i && !p[i - 1] ? i - 1 : i;
        i += 2;
    }
    return n;
}

static void enc_deliver(struct comp *c)
{
    struct port *out = &c->port[OUTP];
    while (running(c, OUTP) && c->npackets && out->q.n) {
        struct packet *k = &c->packets[0];
        OMX_BUFFERHEADERTYPE *b = pop(&out->q);
        /* the stream's parameter sets go first, on their own: the driver puts them in front of the first frame */
        size_t len = k->len - k->off;
        OMX_U32 flags = OMX_BUFFERFLAG_ENDOFFRAME;
        if (!c->config_sent) {
            size_t pic = k->config ? k->len : first_picture(c, k->data, k->len);
            if (pic > k->off) { len = pic - k->off; flags |= OMX_BUFFERFLAG_CODECCONFIG; }
            c->config_sent = 1;
        }
        if (!(flags & OMX_BUFFERFLAG_CODECCONFIG) && k->key) flags |= OMX_BUFFERFLAG_SYNCFRAME;
        if (len > b->nAllocLen) {
            LOGE("%s: %zu-byte packet in a %u-byte buffer", c->kind->name, len, b->nAllocLen);
            len = b->nAllocLen;
        }
        memcpy(b->pBuffer, k->data + k->off, len);
        c->stat_bytes += len;
        b->nOffset = 0;
        b->nFilledLen = len;
        b->nTimeStamp = k->ts;
        b->nFlags = flags;
        k->off += len;
        if (k->off >= k->len) {
            v4l2enc_release_packet(c->enc, k->index);
            memmove(c->packets, c->packets + 1, --c->npackets * sizeof(*c->packets));
        }
        return_buffer(c, b);
    }
    if (running(c, OUTP) && c->eos_out && !c->npackets && out->q.n) {
        OMX_BUFFERHEADERTYPE *b = pop(&out->q);
        b->nFilledLen = 0; b->nOffset = 0;
        b->nFlags = OMX_BUFFERFLAG_EOS;
        c->eos_out = 0; c->draining = 0;
        log_encoder_stats(c, "end of stream");
        c->stat_frames = 0;
        return_buffer(c, b);
        emit_event(c, OMX_EventBufferFlag, OUTP, OMX_BUFFERFLAG_EOS);
        /* a new stream after this one starts on a fresh encoder */
        if (c->enc) stop_encoder(c);
    }
}

static void *worker(void *arg)
{
    struct comp *c = arg;
    pthread_mutex_lock(&c->lock);
    int idle_spin = 0;
    while (!c->quit) {
        while (c->ncmd) {
            struct cmd k = c->cmds[0];
            memmove(c->cmds, c->cmds + 1, --c->ncmd * sizeof(*c->cmds));
            run_cmd(c, k);
        }
        settle(c);
        if (c->enc && c->state == OMX_StateExecuting) {
            int n = v4l2enc_poll(c->enc);
            if (n < 0 || c->error) {
                emit_event(c, OMX_EventError, OMX_ErrorHardware, 0);
                c->error = 0;
                stop_encoder(c);
            }
            idle_spin = n == 0;
        }
        if (c->kind->enc && c->state == OMX_StateExecuting) {
            enc_feed(c);
            enc_deliver(c);
        }
        if (c->dec && c->state == OMX_StateExecuting) {
            int n = v4l2dec_poll(c->dec, 0);
            if (n < 0 || c->error) {
                emit_event(c, OMX_EventError, OMX_ErrorHardware, 0);
                c->error = 0;
                stop_decoder(c);
            }
            idle_spin = n == 0;
            feed(c);
            deliver(c);
        }
        if (c->ncmd || c->quit) continue;
        int fd = c->dec ? v4l2dec_fd(c->dec) : c->enc ? v4l2enc_fd(c->enc) : -1;
        struct pollfd p[2] = {{c->wake, POLLIN, 0}, {fd, POLLIN | POLLOUT | POLLPRI, 0}};
        int busy = fd >= 0 && c->state == OMX_StateExecuting;
        pthread_mutex_unlock(&c->lock);
        /* the driver's poll flags are only a hint and can stay raised: after a pass that found nothing, nap briefly */
        if (busy && idle_spin) poll(p, 1, 2);
        else poll(p, busy ? 2 : 1, busy ? 100 : -1);
        uint64_t v;
        if (p[0].revents & POLLIN && read(c->wake, &v, 8) < 0) {}
        pthread_mutex_lock(&c->lock);
    }
    pthread_mutex_unlock(&c->lock);
    return 0;
}

/* ---- OMX component entry points ---- */

#define COMP(h) ((struct comp *)((OMX_COMPONENTTYPE *)(h))->pComponentPrivate)

static OMX_ERRORTYPE GetComponentVersion(OMX_HANDLETYPE h, OMX_STRING name, OMX_VERSIONTYPE *cv, OMX_VERSIONTYPE *sv,
    OMX_UUIDTYPE *uuid)
{
    struct comp *c = COMP(h);
    strncpy(name, c->kind->name, OMX_MAX_STRINGNAME_SIZE - 1);
    name[OMX_MAX_STRINGNAME_SIZE - 1] = 0;
    cv->nVersion = OMX_VERSION; sv->nVersion = OMX_VERSION;
    if (uuid) memset(uuid, 0, sizeof(*uuid));
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE SendCommand(OMX_HANDLETYPE h, OMX_COMMANDTYPE cmd, OMX_U32 param, OMX_PTR data)
{
    (void)data;
    struct comp *c = COMP(h);
    pthread_mutex_lock(&c->lock);
    OMX_ERRORTYPE e = OMX_ErrorNone;
    if (c->ncmd == sizeof(c->cmds) / sizeof(c->cmds[0])) e = OMX_ErrorInsufficientResources;
    else c->cmds[c->ncmd++] = (struct cmd){cmd, param};
    pthread_mutex_unlock(&c->lock);
    kick(c);
    return e;
}

static int port_ok(OMX_U32 i) { return i <= OUTP; }
/* the port with frames on it: a decoder's output, an encoder's input */
static OMX_U32 raw_port(const struct comp *c) { return c->kind->enc ? IN : OUTP; }

static OMX_ERRORTYPE GetParameter(OMX_HANDLETYPE h, OMX_INDEXTYPE index, OMX_PTR param)
{
    struct comp *c = COMP(h);
    if (!param) return OMX_ErrorBadParameter;
    OMX_ERRORTYPE e = OMX_ErrorNone;
    pthread_mutex_lock(&c->lock);
    switch ((int)index) {
    case OMX_IndexParamPortDefinition: {
        OMX_PARAM_PORTDEFINITIONTYPE *d = param;
        if (!port_ok(d->nPortIndex)) { e = OMX_ErrorBadPortIndex; break; }
        memcpy(d, &c->port[d->nPortIndex].def, sizeof(*d));
        break;
    }
    case OMX_IndexParamVideoInit: {
        OMX_PORT_PARAM_TYPE *p = param;
        p->nPorts = 2; p->nStartPortNumber = 0;
        break;
    }
    case OMX_IndexParamVideoPortFormat: {
        OMX_VIDEO_PARAM_PORTFORMATTYPE *f = param;
        if (!port_ok(f->nPortIndex)) { e = OMX_ErrorBadPortIndex; break; }
        if (f->nIndex) { e = OMX_ErrorNoMore; break; }
        OMX_VIDEO_PORTDEFINITIONTYPE *v = &c->port[f->nPortIndex].def.format.video;
        f->eCompressionFormat = v->eCompressionFormat;
        f->eColorFormat = v->eColorFormat;
        f->xFramerate = v->xFramerate;
        break;
    }
    case OMX_IndexCodecVideoPortFormat: {
        struct CodecVideoPortFormatParam *f = param;
        if (!port_ok(f->portIndex)) { e = OMX_ErrorBadPortIndex; break; }
        OMX_VIDEO_PORTDEFINITIONTYPE *v = &c->port[f->portIndex].def.format.video;
        if (c->kind->enc && f->portIndex == IN && f->codecColorIndex < 2) {
            /* the encoder takes NV12 and NV21 */
            f->codecColorFormat = f->codecColorIndex ? GRAPHIC_PIXEL_FMT_YCRCB_420_SP : GRAPHIC_PIXEL_FMT_YCBCR_420_SP;
            f->codecCompressFormat = OMX_VIDEO_CodingUnused;
            f->framerate = v->xFramerate;
            break;
        }
        if (f->codecColorIndex) { e = OMX_ErrorNoMore; break; }
        f->codecColorFormat = f->portIndex == raw_port(c) ? (uint32_t)v->eColorFormat : 0;
        f->codecCompressFormat = v->eCompressionFormat;
        f->framerate = v->xFramerate;
        break;
    }
    case OMX_IndexParamSupportBufferType: {
        struct SupportBufferType *t = param;
        if (!port_ok(t->portIndex)) { e = OMX_ErrorBadPortIndex; break; }
        if (c->kind->enc)
            t->bufferTypes = t->portIndex == IN ? CODEC_BUFFER_TYPE_DYNAMIC_HANDLE | CODEC_BUFFER_TYPE_HANDLE
                                                : CODEC_BUFFER_TYPE_AVSHARE_MEM_FD;
        else t->bufferTypes = t->portIndex == IN ? CODEC_BUFFER_TYPE_AVSHARE_MEM_FD : CODEC_BUFFER_TYPE_HANDLE;
        break;
    }
    case OMX_IndexParamGetBufferHandleUsage: {
        struct GetBufferHandleUsageParams *u = param;
        u->usage = BUFFER_USAGE_CPU_READ | BUFFER_USAGE_CPU_WRITE | BUFFER_USAGE_MEM_DMA;
        break;
    }
    case OMX_IndexParamStandardComponentRole: {
        OMX_PARAM_COMPONENTROLETYPE *r = param;
        strncpy((char *)r->cRole, c->kind->role, OMX_MAX_STRINGNAME_SIZE - 1);
        break;
    }
    case OMX_IndexConfigCommonOutputCrop: {
        OMX_CONFIG_RECTTYPE *r = param;
        r->nLeft = r->nTop = 0;
        r->nWidth = c->port[OUTP].def.format.video.nFrameWidth;
        r->nHeight = c->port[OUTP].def.format.video.nFrameHeight;
        break;
    }
    case OMX_IndexParamVideoProfileLevelQuerySupported: {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE *p = param;
        if (c->kind->enc && c->kind->codec == V4L2DEC_H264 && p->nProfileIndex < 3) {
            static const OMX_VIDEO_AVCPROFILETYPE avc[] = {OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCProfileMain,
                OMX_VIDEO_AVCProfileBaseline};
            p->eProfile = avc[p->nProfileIndex]; p->eLevel = OMX_VIDEO_AVCLevel51;
            break;
        }
        if (p->nProfileIndex) { e = OMX_ErrorNoMore; break; }
        if (c->kind->enc && c->kind->codec == V4L2DEC_HEVC) {
            p->eProfile = CODEC_HEVC_PROFILE_MAIN; p->eLevel = CODEC_HEVC_MAIN_TIER_LEVEL51;
            break;
        }
        if (c->kind->codec == V4L2DEC_H264) { p->eProfile = OMX_VIDEO_AVCProfileHigh; p->eLevel = OMX_VIDEO_AVCLevel51; }
        else { p->eProfile = CODEC_HEVC_PROFILE_MAIN10; p->eLevel = CODEC_HEVC_MAIN_TIER_LEVEL51; }
        break;
    }
    case OMX_IndexParamVideoBitrate: {
        OMX_VIDEO_PARAM_BITRATETYPE *b = param;
        b->eControlRate = c->cbr ? OMX_Video_ControlRateConstant : OMX_Video_ControlRateVariable;
        b->nTargetBitrate = c->bitrate;
        break;
    }
    case OMX_IndexParamVideoAvc: {
        OMX_VIDEO_PARAM_AVCTYPE *a = param;
        OMX_U32 port = a->nPortIndex;
        memset((char *)a + offsetof(OMX_VIDEO_PARAM_AVCTYPE, nSliceHeaderSpacing), 0,
            sizeof(*a) - offsetof(OMX_VIDEO_PARAM_AVCTYPE, nSliceHeaderSpacing));
        a->nPortIndex = port;
        a->nPFrames = c->gop ? c->gop - 1 : 29;
        a->nRefFrames = 1;
        a->bEnableFMO = a->bEnableASO = a->bEnableRS = OMX_FALSE;
        a->eProfile = OMX_VIDEO_AVCProfileHigh;
        a->eLevel = OMX_VIDEO_AVCLevel41;
        a->nAllowedPictureTypes = OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;
        a->bFrameMBsOnly = OMX_TRUE;
        a->bEntropyCodingCABAC = OMX_TRUE;
        a->bconstIpred = OMX_TRUE;
        a->eLoopFilterMode = OMX_VIDEO_AVCLoopFilterEnable;
        break;
    }
    case OMX_IndexParamVideoHevc: {
        struct CodecVideoParamHevc *h = param;
        h->profile = CODEC_HEVC_PROFILE_MAIN;
        h->level = CODEC_HEVC_MAIN_TIER_LEVEL41;
        h->keyFrameInterval = c->gop ? c->gop : 30;
        break;
    }
    default:
        LOGW("%s: GetParameter 0x%x unsupported", c->kind->name, index);
        e = OMX_ErrorUnsupportedIndex;
    }
    pthread_mutex_unlock(&c->lock);
    return e;
}

static OMX_ERRORTYPE SetParameter(OMX_HANDLETYPE h, OMX_INDEXTYPE index, OMX_PTR param)
{
    struct comp *c = COMP(h);
    if (!param) return OMX_ErrorBadParameter;
    OMX_ERRORTYPE e = OMX_ErrorNone;
    pthread_mutex_lock(&c->lock);
    switch ((int)index) {
    case OMX_IndexParamPortDefinition: {
        OMX_PARAM_PORTDEFINITIONTYPE *d = param;
        if (!port_ok(d->nPortIndex)) { e = OMX_ErrorBadPortIndex; break; }
        struct port *p = &c->port[d->nPortIndex];
        OMX_VIDEO_PORTDEFINITIONTYPE *v = &p->def.format.video;
        if (d->nBufferCountActual >= p->def.nBufferCountMin && d->nBufferCountActual <= MAXBUF)
            p->def.nBufferCountActual = d->nBufferCountActual;
        if (c->kind->enc) {
            /* both ports have the frame's size; the input's buffers may be bigger, the output's bitrate is ours */
            if (d->format.video.nFrameWidth && d->format.video.nFrameHeight)
                set_encoder_geometry(c, d->format.video.nFrameWidth, d->format.video.nFrameHeight);
            if (d->format.video.xFramerate)
                c->port[IN].def.format.video.xFramerate = c->port[OUTP].def.format.video.xFramerate =
                    d->format.video.xFramerate;
            if (d->nPortIndex == IN && d->nBufferSize > p->def.nBufferSize) p->def.nBufferSize = d->nBufferSize;
            if (d->nPortIndex == OUTP && d->format.video.nBitrate) c->bitrate = d->format.video.nBitrate;
        } else if (d->nPortIndex == IN) {
            if (d->format.video.nFrameWidth) v->nFrameWidth = v->nStride = d->format.video.nFrameWidth;
            if (d->format.video.nFrameHeight) v->nFrameHeight = v->nSliceHeight = d->format.video.nFrameHeight;
            if (d->format.video.xFramerate) v->xFramerate = d->format.video.xFramerate;
            if (d->nBufferSize > p->def.nBufferSize) p->def.nBufferSize = d->nBufferSize;
            /* the output follows the input's size until the stream says otherwise */
            set_output_geometry(c, v->nFrameWidth, v->nFrameHeight);
        } else {
            if (d->format.video.nFrameWidth && d->format.video.nFrameHeight)
                set_output_geometry(c, d->format.video.nFrameWidth, d->format.video.nFrameHeight);
            if ((OMX_U32)d->format.video.nStride >= v->nFrameWidth) v->nStride = d->format.video.nStride;
            if (d->format.video.nSliceHeight >= v->nFrameHeight) v->nSliceHeight = d->format.video.nSliceHeight;
            p->def.nBufferSize = v->nStride * v->nSliceHeight * 3 / 2;
            if (d->format.video.xFramerate) v->xFramerate = d->format.video.xFramerate;
        }
        break;
    }
    case OMX_IndexParamVideoPortFormat: {
        OMX_VIDEO_PARAM_PORTFORMATTYPE *f = param;
        if (!port_ok(f->nPortIndex)) { e = OMX_ErrorBadPortIndex; break; }
        if (f->nPortIndex == raw_port(c) && f->eColorFormat != c->port[raw_port(c)].def.format.video.eColorFormat)
            e = OMX_ErrorUnsupportedSetting;
        break;
    }
    case OMX_IndexCodecVideoPortFormat: {
        struct CodecVideoPortFormatParam *f = param;
        if (f->portIndex != raw_port(c)) break;
        if (f->codecColorFormat != GRAPHIC_PIXEL_FMT_YCBCR_420_SP && f->codecColorFormat != GRAPHIC_PIXEL_FMT_YCRCB_420_SP) {
            LOGW("%s: colour format %u unsupported", c->kind->name, f->codecColorFormat);
            e = OMX_ErrorUnsupportedSetting;
            break;
        }
        c->nv21 = f->codecColorFormat == GRAPHIC_PIXEL_FMT_YCRCB_420_SP;
        c->port[raw_port(c)].def.format.video.eColorFormat = (OMX_COLOR_FORMATTYPE)f->codecColorFormat;
        break;
    }
    case OMX_IndexParamUseBufferType: {
        struct UseBufferType *t = param;
        if (!port_ok(t->portIndex)) { e = OMX_ErrorBadPortIndex; break; }
        LOGI("%s: port %u buffer type 0x%x", c->kind->name, t->portIndex, t->bufferType);
        c->port[t->portIndex].type = t->bufferType;
        break;
    }
    case OMX_IndexParamVideoBitrate: {
        OMX_VIDEO_PARAM_BITRATETYPE *b = param;
        if (b->nTargetBitrate) c->bitrate = b->nTargetBitrate;
        c->cbr = b->eControlRate == OMX_Video_ControlRateConstant || b->eControlRate == OMX_Video_ControlRateConstantSkipFrames;
        LOGI("%s: bitrate %u mode %d", c->kind->name, c->bitrate, b->eControlRate);
        break;
    }
    case OMX_IndexParamVideoAvc: {
        OMX_VIDEO_PARAM_AVCTYPE *a = param;
        /* nPFrames: P frames between I frames; HCodec's "only the first" is near UINT32_MAX (the driver's choice) */
        c->gop = a->nPFrames >= 0x7fffffff ? 0 : a->nPFrames + 1;
        LOGI("%s: avc profile 0x%x level 0x%x gop %u", c->kind->name, a->eProfile, a->eLevel, c->gop);
        break;
    }
    case OMX_IndexParamVideoHevc: {
        struct CodecVideoParamHevc *h = param;
        c->gop = h->keyFrameInterval >= 0x7fffffff ? 0 : h->keyFrameInterval;
        LOGI("%s: hevc profile %d level %d gop %u", c->kind->name, h->profile, h->level, c->gop);
        break;
    }
    case OMX_IndexParamStandardComponentRole:
    case OMX_IndexParamProcessName:
    case OMX_IndexParamWorkingFrequency:
    case OMX_IndexParamOperatingRate:
    case OMX_IndexParamLowLatency:
    case OMX_IndexColorAspects:
        break;
    default:
        LOGW("%s: SetParameter 0x%x unsupported", c->kind->name, index);
        /* the encoder's optional features (LTR, QP ranges, layers...): left at the driver's defaults */
        if (!c->kind->enc) e = OMX_ErrorUnsupportedIndex;
    }
    pthread_mutex_unlock(&c->lock);
    return e;
}

static OMX_ERRORTYPE GetConfig(OMX_HANDLETYPE h, OMX_INDEXTYPE index, OMX_PTR config)
{
    if (index == OMX_IndexConfigCommonOutputCrop) return GetParameter(h, index, config);
    LOGW("%s: GetConfig 0x%x unsupported", COMP(h)->kind->name, index);
    return OMX_ErrorUnsupportedIndex;
}

static OMX_ERRORTYPE SetConfig(OMX_HANDLETYPE h, OMX_INDEXTYPE index, OMX_PTR config)
{
    struct comp *c = COMP(h);
    if (!config) return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->lock);
    switch ((int)index) {
    case OMX_IndexConfigVideoBitrate: {
        OMX_VIDEO_CONFIG_BITRATETYPE *b = config;
        if (c->kind->enc && b->nEncodeBitrate) {
            c->bitrate = b->nEncodeBitrate;
            if (c->enc) v4l2enc_set_bitrate(c->enc, c->bitrate);
        }
        break;
    }
    case OMX_IndexConfigVideoIntraVOPRefresh: {
        OMX_CONFIG_INTRAREFRESHVOPTYPE *r = config;
        if (c->enc && r->IntraRefreshVOP) v4l2enc_request_keyframe(c->enc);
        break;
    }
    case OMX_IndexParamVideoBitrate:
        pthread_mutex_unlock(&c->lock);
        return SetParameter(h, index, config);
    default:
        LOGW("%s: SetConfig 0x%x ignored", c->kind->name, index);
    }
    pthread_mutex_unlock(&c->lock);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE GetExtensionIndex(OMX_HANDLETYPE h, OMX_STRING name, OMX_INDEXTYPE *index)
{
    (void)index;
    LOGW("%s: GetExtensionIndex %s unsupported", COMP(h)->kind->name, name);
    return OMX_ErrorUnsupportedIndex;
}

static OMX_ERRORTYPE GetState(OMX_HANDLETYPE h, OMX_STATETYPE *state)
{
    struct comp *c = COMP(h);
    pthread_mutex_lock(&c->lock);
    *state = c->state;
    pthread_mutex_unlock(&c->lock);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE add_buffer(struct comp *c, OMX_BUFFERHEADERTYPE **out, OMX_U32 port, OMX_PTR app, OMX_U32 size,
    OMX_U8 *mem)
{
    if (!port_ok(port)) return OMX_ErrorBadPortIndex;
    struct port *p = &c->port[port];
    if (p->nbuf == MAXBUF) return OMX_ErrorInsufficientResources;
    OMX_BUFFERHEADERTYPE *b = calloc(1, sizeof(*b) + sizeof(struct bufpriv));
    if (!b) return OMX_ErrorInsufficientResources;
    init_header(b);
    struct bufpriv *bp = (struct bufpriv *)(b + 1);
    if (!mem) {
        mem = calloc(1, size);
        if (!mem) { free(b); return OMX_ErrorInsufficientResources; }
        bp->own = 1;
    }
    bp->mapfd = -1;
    bp->port = port;
    b->pBuffer = mem;
    b->nAllocLen = size;
    b->pAppPrivate = app;
    b->pPlatformPrivate = bp;
    b->nInputPortIndex = port == IN ? IN : OMX_ALL;
    b->nOutputPortIndex = port == OUTP ? OUTP : OMX_ALL;
    p->buf[p->nbuf++] = b;
    *out = b;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE UseBuffer(OMX_HANDLETYPE h, OMX_BUFFERHEADERTYPE **out, OMX_U32 port, OMX_PTR app, OMX_U32 size,
    OMX_U8 *mem)
{
    struct comp *c = COMP(h);
    if (!mem) return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->lock);
    OMX_ERRORTYPE e = add_buffer(c, out, port, app, size, mem);
    pthread_mutex_unlock(&c->lock);
    kick(c);
    return e;
}

static OMX_ERRORTYPE AllocateBuffer(OMX_HANDLETYPE h, OMX_BUFFERHEADERTYPE **out, OMX_U32 port, OMX_PTR app,
    OMX_U32 size)
{
    struct comp *c = COMP(h);
    pthread_mutex_lock(&c->lock);
    OMX_ERRORTYPE e = add_buffer(c, out, port, app, size, 0);
    pthread_mutex_unlock(&c->lock);
    kick(c);
    return e;
}

static OMX_ERRORTYPE FreeBuffer(OMX_HANDLETYPE h, OMX_U32 port, OMX_BUFFERHEADERTYPE *b)
{
    struct comp *c = COMP(h);
    if (!port_ok(port) || !b) return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->lock);
    struct port *p = &c->port[port];
    unsigned i;
    for (i = 0; i < p->nbuf && p->buf[i] != b; i++) {}
    if (i == p->nbuf) { pthread_mutex_unlock(&c->lock); return OMX_ErrorBadParameter; }
    p->buf[i] = p->buf[--p->nbuf];
    /* drop it from the queue too, should the client free a buffer it never got back */
    struct fifo keep = {0};
    OMX_BUFFERHEADERTYPE *q;
    while ((q = pop(&p->q))) if (q != b) push(&keep, q);
    p->q = keep;
    pthread_mutex_unlock(&c->lock);
    struct bufpriv *bp = b->pPlatformPrivate;
    if (bp->map) munmap(bp->map, bp->maplen);
    if (bp->own) free(b->pBuffer);
    free(b);
    kick(c);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE queue_buffer(OMX_HANDLETYPE h, OMX_BUFFERHEADERTYPE *b, int port)
{
    struct comp *c = COMP(h);
    if (!b) return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->lock);
    OMX_ERRORTYPE e = OMX_ErrorNone;
    if (c->state != OMX_StateExecuting && c->state != OMX_StatePause && c->state != OMX_StateIdle)
        e = OMX_ErrorIncorrectStateOperation;
    else push(&c->port[port].q, b);
    pthread_mutex_unlock(&c->lock);
    kick(c);
    return e;
}

static OMX_ERRORTYPE EmptyThisBuffer(OMX_HANDLETYPE h, OMX_BUFFERHEADERTYPE *b) { return queue_buffer(h, b, IN); }
static OMX_ERRORTYPE FillThisBuffer(OMX_HANDLETYPE h, OMX_BUFFERHEADERTYPE *b) { return queue_buffer(h, b, OUTP); }

static OMX_ERRORTYPE SetCallbacks(OMX_HANDLETYPE h, OMX_CALLBACKTYPE *cb, OMX_PTR app)
{
    struct comp *c = COMP(h);
    pthread_mutex_lock(&c->lock);
    c->cb = *cb; c->app = app;
    pthread_mutex_unlock(&c->lock);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE ComponentRoleEnum(OMX_HANDLETYPE h, OMX_U8 *role, OMX_U32 index)
{
    if (index) return OMX_ErrorNoMore;
    strncpy((char *)role, COMP(h)->kind->role, OMX_MAX_STRINGNAME_SIZE - 1);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE ComponentDeInit(OMX_HANDLETYPE h) { (void)h; return OMX_ErrorNone; }
static OMX_ERRORTYPE NotImplemented(void) { return OMX_ErrorNotImplemented; }

/* ---- the core ---- */

OMX_ERRORTYPE OMX_Init(void) { return OMX_ErrorNone; }
OMX_ERRORTYPE OMX_Deinit(void) { return OMX_ErrorNone; }

OMX_ERRORTYPE OMX_ComponentNameEnum(OMX_STRING name, OMX_U32 len, OMX_U32 index)
{
    if (index >= NKINDS) return OMX_ErrorNoMore;
    snprintf(name, len, "%s", kinds[index].name);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_GetRolesOfComponent(OMX_STRING name, OMX_U32 *n, OMX_U8 **roles)
{
    for (unsigned i = 0; i < NKINDS; i++)
        if (!strcmp(name, kinds[i].name)) {
            if (roles && *n >= 1 && roles[0]) strncpy((char *)roles[0], kinds[i].role, OMX_MAX_STRINGNAME_SIZE - 1);
            *n = 1;
            return OMX_ErrorNone;
        }
    return OMX_ErrorComponentNotFound;
}

OMX_ERRORTYPE OMX_GetComponentsOfRole(OMX_STRING role, OMX_U32 *n, OMX_U8 **names)
{
    OMX_U32 k = 0;
    for (unsigned i = 0; i < NKINDS; i++)
        if (!strcmp(role, kinds[i].role)) {
            if (names && k < *n && names[k]) strncpy((char *)names[k], kinds[i].name, OMX_MAX_STRINGNAME_SIZE - 1);
            k++;
        }
    *n = k;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_GetHandle(OMX_HANDLETYPE *out, OMX_STRING name, OMX_PTR app, OMX_CALLBACKTYPE *cb)
{
    const struct kind *k = 0;
    for (unsigned i = 0; i < NKINDS; i++) if (!strcmp(name, kinds[i].name)) k = &kinds[i];
    if (!k) { LOGW("OMX_GetHandle %s: not ours", name); return OMX_ErrorComponentNotFound; }
    const char *dev = k->enc ? "/dev/video33" : "/dev/video32";
    if (access(dev, R_OK | W_OK)) {
        LOGE("OMX_GetHandle %s: %s: %s", name, dev, strerror(errno));
        return OMX_ErrorInsufficientResources;
    }
    OMX_COMPONENTTYPE *h = calloc(1, sizeof(*h));
    struct comp *c = calloc(1, sizeof(*c));
    if (!h || !c) { free(h); free(c); return OMX_ErrorInsufficientResources; }
    c->h = h; c->kind = k; c->app = app;
    if (cb) c->cb = *cb;
    c->state = c->target = OMX_StateLoaded;
    init_ports(c);
    pthread_mutex_init(&c->lock, 0);
    c->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    h->nSize = sizeof(*h);
    h->nVersion.nVersion = OMX_VERSION;
    h->pComponentPrivate = c;
    h->pApplicationPrivate = app;
    h->GetComponentVersion = GetComponentVersion;
    h->SendCommand = SendCommand;
    h->GetParameter = GetParameter;
    h->SetParameter = SetParameter;
    h->GetConfig = GetConfig;
    h->SetConfig = SetConfig;
    h->GetExtensionIndex = GetExtensionIndex;
    h->GetState = GetState;
    h->ComponentTunnelRequest = (void *)NotImplemented;
    h->UseBuffer = UseBuffer;
    h->AllocateBuffer = AllocateBuffer;
    h->FreeBuffer = FreeBuffer;
    h->EmptyThisBuffer = EmptyThisBuffer;
    h->FillThisBuffer = FillThisBuffer;
    h->SetCallbacks = SetCallbacks;
    h->ComponentDeInit = ComponentDeInit;
    h->UseEGLImage = (void *)NotImplemented;
    h->ComponentRoleEnum = ComponentRoleEnum;
    if (c->wake < 0 || pthread_create(&c->thread, 0, worker, c)) {
        if (c->wake >= 0) close(c->wake);
        free(c); free(h);
        return OMX_ErrorInsufficientResources;
    }
    LOGI("OMX_GetHandle %s", name);
    *out = h;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_FreeHandle(OMX_HANDLETYPE handle)
{
    if (!handle) return OMX_ErrorBadParameter;
    struct comp *c = COMP(handle);
    pthread_mutex_lock(&c->lock);
    c->quit = 1;
    pthread_mutex_unlock(&c->lock);
    kick(c);
    pthread_join(c->thread, 0);
    if (c->dec) stop_decoder(c);
    if (c->enc) stop_encoder(c);
    for (int i = 0; i < 2; i++)
        for (unsigned j = 0; j < c->port[i].nbuf; j++) {
            OMX_BUFFERHEADERTYPE *b = c->port[i].buf[j];
            struct bufpriv *bp = b->pPlatformPrivate;
            if (bp->map) munmap(bp->map, bp->maplen);
            if (bp->own) free(b->pBuffer);
            free(b);
        }
    close(c->wake);
    pthread_mutex_destroy(&c->lock);
    LOGI("OMX_FreeHandle %s", c->kind->name);
    free(c);
    free(handle);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_SetupTunnel(OMX_HANDLETYPE out, OMX_U32 op, OMX_HANDLETYPE in, OMX_U32 ip)
{
    (void)out; (void)op; (void)in; (void)ip;
    return OMX_ErrorNotImplemented;
}
