/* SPDX-License-Identifier: MIT
 * asteroids: stateful V4L2 encoding on Qualcomm's msm_video, see v4l2enc.h. */
#include "v4l2enc.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define NOUT 8
#define NCAP 8

struct slot { int fd; void *map; size_t len; int queued; uint64_t cookie; };

struct v4l2enc {
    int fd, heap;
    enum v4l2enc_codec codec;
    const struct v4l2enc_ops *ops;
    void *user;
    struct slot in[NOUT], cap[NCAP];
    unsigned nin, ncap, stride, scanlines;
    int stopped;                      /* 1 draining, 2 drained */
};

static void logf_(struct v4l2enc *e, const char *fmt, ...)
{
    if (!e->ops->log) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    e->ops->log(e->user, msg);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : 0;
}

static int alloc_slot(struct v4l2enc *e, struct slot *s, size_t len)
{
    struct dma_heap_allocation_data a = {.len = len, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(e->heap, DMA_HEAP_IOCTL_ALLOC, &a)) return -errno;
    void *map = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
    if (map == MAP_FAILED) { int r = -errno; close(a.fd); return r; }
    s->fd = a.fd; s->map = map; s->len = len; s->queued = 0;
    return 0;
}

static void free_slot(struct slot *s)
{
    if (s->map && s->map != MAP_FAILED) munmap(s->map, s->len);
    if (s->fd > 0) close(s->fd);
    memset(s, 0, sizeof(*s));
}

static void buf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync s = {flags};
    xioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

static int set_ctrl(struct v4l2enc *e, unsigned id, int value, const char *name)
{
    struct v4l2_control c = {.id = id, .value = value};
    int r = xioctl(e->fd, VIDIOC_S_CTRL, &c);
    if (r) logf_(e, "%s = %d: %d", name, value, r);
    return r;
}
#define CTRL(e, id, v) set_ctrl(e, id, v, #id)

static int queue_capture(struct v4l2enc *e, unsigned i)
{
    struct v4l2_plane pl = {.m.fd = e->cap[i].fd, .length = e->cap[i].len};
    struct v4l2_buffer b = {.type = CAP, .memory = V4L2_MEMORY_DMABUF, .index = i, .m.planes = &pl, .length = 1};
    int r = xioctl(e->fd, VIDIOC_QBUF, &b);
    if (!r) e->cap[i].queued = 1;
    return r;
}

struct v4l2enc *v4l2enc_open(const struct v4l2enc_config *cfg, const struct v4l2enc_ops *ops, void *user)
{
    struct v4l2enc *e = calloc(1, sizeof(*e));
    if (!e) return 0;
    int line = 0;
    e->ops = ops; e->user = user; e->codec = cfg->codec;
    e->heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    e->fd = open("/dev/video33", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (e->heap < 0 || e->fd < 0) { line = __LINE__; goto fail; }
    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_EOS};
    xioctl(e->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    /* the bitstream first: the frame format and its limits follow from it */
    struct v4l2_format c = {.type = CAP};
    c.fmt.pix_mp.pixelformat = cfg->codec == V4L2ENC_HEVC ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    c.fmt.pix_mp.width = cfg->width;
    c.fmt.pix_mp.height = cfg->height;
    c.fmt.pix_mp.num_planes = 1;
    if (xioctl(e->fd, VIDIOC_S_FMT, &c) || xioctl(e->fd, VIDIOC_G_FMT, &c)) { line = __LINE__; goto fail; }
    struct v4l2_format f = {.type = OUT};
    f.fmt.pix_mp.pixelformat = cfg->nv21 ? V4L2_PIX_FMT_NV21 : V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.width = cfg->width;
    f.fmt.pix_mp.height = cfg->height;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(e->fd, VIDIOC_S_FMT, &f) || xioctl(e->fd, VIDIOC_G_FMT, &f)) { line = __LINE__; goto fail; }
    e->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    /* msm_vidc's linear NV12: chroma after the luma rows rounded up to 32 (the frame's own height may not say so) */
    e->scanlines = (f.fmt.pix_mp.height + 31) & ~31u;
    if ((size_t)e->stride * e->scanlines * 3 / 2 > f.fmt.pix_mp.plane_fmt[0].sizeimage)
        e->scanlines = f.fmt.pix_mp.height;
    struct v4l2_selection sel = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .target = V4L2_SEL_TGT_CROP,
        .r = {0, 0, cfg->width, cfg->height}};
    xioctl(e->fd, VIDIOC_S_SELECTION, &sel);
    logf_(e, "%ux%u in: stride %u scanlines %u size %u, out: %u bytes", f.fmt.pix_mp.width,
        f.fmt.pix_mp.height, e->stride, e->scanlines, f.fmt.pix_mp.plane_fmt[0].sizeimage,
        c.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_streamparm parm = {.type = OUT};
    parm.parm.output.timeperframe.numerator = 1 << 16;
    parm.parm.output.timeperframe.denominator = cfg->fps_q16 ? cfg->fps_q16 : 30 << 16;
    xioctl(e->fd, VIDIOC_S_PARM, &parm);
    parm.type = CAP;
    parm.parm.capture.timeperframe = parm.parm.output.timeperframe;
    xioctl(e->fd, VIDIOC_S_PARM, &parm);

    /* CBR whatever the client asked for: msm_vidc's VBR overshoots (54 Mbit/s from the camera for 17 asked, its peak
     * control changes nothing), while its CBR keeps to the bitrate and doesn't pad simple pictures */
    CTRL(e, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    if (cfg->bitrate) CTRL(e, V4L2_CID_MPEG_VIDEO_BITRATE, cfg->bitrate);
    if (cfg->gop) CTRL(e, V4L2_CID_MPEG_VIDEO_GOP_SIZE, cfg->gop);
    CTRL(e, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
    CTRL(e, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE);
    CTRL(e, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);
    if (cfg->codec == V4L2ENC_HEVC) CTRL(e, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
    else CTRL(e, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);

    /* the buffer sizes depend on the controls (the bitstream's on the bitrate): read them again */
    if (xioctl(e->fd, VIDIOC_G_FMT, &c) || xioctl(e->fd, VIDIOC_G_FMT, &f)) { line = __LINE__; goto fail; }
    logf_(e, "buffers: in %u bytes, out %u bytes", f.fmt.pix_mp.plane_fmt[0].sizeimage,
        c.fmt.pix_mp.plane_fmt[0].sizeimage);
    struct v4l2_requestbuffers rb = {.count = NOUT, .type = OUT, .memory = V4L2_MEMORY_DMABUF};
    if (xioctl(e->fd, VIDIOC_REQBUFS, &rb)) { line = __LINE__; goto fail; }
    e->nin = rb.count < NOUT ? rb.count : NOUT;
    for (unsigned i = 0; i < e->nin; i++)
        if (alloc_slot(e, &e->in[i], f.fmt.pix_mp.plane_fmt[0].sizeimage)) { line = __LINE__; goto fail; }
    rb = (struct v4l2_requestbuffers){.count = NCAP, .type = CAP, .memory = V4L2_MEMORY_DMABUF};
    if (xioctl(e->fd, VIDIOC_REQBUFS, &rb)) { line = __LINE__; goto fail; }
    e->ncap = rb.count < NCAP ? rb.count : NCAP;
    for (unsigned i = 0; i < e->ncap; i++)
        if (alloc_slot(e, &e->cap[i], c.fmt.pix_mp.plane_fmt[0].sizeimage)) { line = __LINE__; goto fail; }
    /* msm_vidc takes bitstream buffers only once both queues stream */
    int t = OUT;
    if (xioctl(e->fd, VIDIOC_STREAMON, &t)) { line = __LINE__; goto fail; }
    t = CAP;
    if (xioctl(e->fd, VIDIOC_STREAMON, &t)) { line = __LINE__; goto fail; }
    for (unsigned i = 0; i < e->ncap; i++)
        if (queue_capture(e, i)) { line = __LINE__; goto fail; }
    return e;
fail:
    logf_(e, "open failed (v4l2enc.c:%d): %s", line, strerror(errno));
    v4l2enc_close(e);
    return 0;
}

void v4l2enc_close(struct v4l2enc *e)
{
    if (!e) return;
    if (e->fd >= 0) {
        int t = OUT;
        xioctl(e->fd, VIDIOC_STREAMOFF, &t);
        t = CAP;
        xioctl(e->fd, VIDIOC_STREAMOFF, &t);
        struct v4l2_requestbuffers rb = {.count = 0, .type = OUT, .memory = V4L2_MEMORY_DMABUF};
        xioctl(e->fd, VIDIOC_REQBUFS, &rb);
        rb.type = CAP;
        xioctl(e->fd, VIDIOC_REQBUFS, &rb);
    }
    for (unsigned i = 0; i < e->nin; i++) free_slot(&e->in[i]);
    for (unsigned i = 0; i < e->ncap; i++) free_slot(&e->cap[i]);
    if (e->fd >= 0) close(e->fd);
    if (e->heap >= 0) close(e->heap);
    free(e);
}

int v4l2enc_fd(struct v4l2enc *e) { return e->fd; }

void v4l2enc_layout(struct v4l2enc *e, unsigned *stride, unsigned *scanlines)
{
    *stride = e->stride;
    *scanlines = e->scanlines;
}

int v4l2enc_get_input(struct v4l2enc *e, uint8_t **data)
{
    for (unsigned i = 0; i < e->nin; i++)
        if (!e->in[i].queued) {
            buf_sync(e->in[i].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            *data = e->in[i].map;
            return (int)i;
        }
    return -EAGAIN;
}

int v4l2enc_queue_input(struct v4l2enc *e, int index, int64_t timestamp, uint64_t cookie)
{
    if (index < 0 || (unsigned)index >= e->nin || e->in[index].queued) return -EINVAL;
    struct slot *s = &e->in[index];
    buf_sync(s->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    if (e->stopped) {
        struct v4l2_encoder_cmd cmd = {.cmd = V4L2_ENC_CMD_START};
        xioctl(e->fd, VIDIOC_ENCODER_CMD, &cmd);
        e->stopped = 0;
    }
    struct v4l2_plane pl = {.m.fd = s->fd, .length = s->len, .bytesused = s->len};
    struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_DMABUF, .index = index, .m.planes = &pl, .length = 1};
    b.timestamp.tv_sec = timestamp / 1000000;
    b.timestamp.tv_usec = timestamp % 1000000;
    int r = xioctl(e->fd, VIDIOC_QBUF, &b);
    if (r) return r;
    s->queued = 1; s->cookie = cookie;
    return 0;
}

int v4l2enc_request_keyframe(struct v4l2enc *e) { return CTRL(e, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1); }
int v4l2enc_set_bitrate(struct v4l2enc *e, unsigned bitrate) { return CTRL(e, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate); }

int v4l2enc_drain(struct v4l2enc *e)
{
    struct v4l2_encoder_cmd cmd = {.cmd = V4L2_ENC_CMD_STOP};
    int r = xioctl(e->fd, VIDIOC_ENCODER_CMD, &cmd);
    if (!r) e->stopped = 1;
    return r;
}

void v4l2enc_release_packet(struct v4l2enc *e, unsigned index)
{
    if (index >= e->ncap || e->cap[index].queued) return;
    buf_sync(e->cap[index].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    queue_capture(e, index);
}

/* Whether Annex-B data holds a picture (a VCL NAL unit), not only parameter sets and SEI. */
static int has_picture(enum v4l2enc_codec codec, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        uint8_t h = p[i + 3];
        if (codec == V4L2ENC_H264 ? (h & 0x1f) >= 1 && (h & 0x1f) <= 5 : ((h >> 1) & 0x3f) < 32) return 1;
        i += 2;
    }
    return 0;
}

int v4l2enc_poll(struct v4l2enc *e)
{
    /* as for the decoder, msm_vidc's poll flags are only a hint: every queue is simply tried */
    int progress = 0;
    struct v4l2_event ev;
    while (!xioctl(e->fd, VIDIOC_DQEVENT, &ev)) progress++;
    struct v4l2_plane pl = {0};
    struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1};
    while (!xioctl(e->fd, VIDIOC_DQBUF, &b)) {
        progress++;
        if (b.index < e->nin && e->in[b.index].queued) {
            e->in[b.index].queued = 0;
            if (e->ops->input_done) e->ops->input_done(e->user, e->in[b.index].cookie);
        }
    }
    for (;;) {
        memset(&pl, 0, sizeof(pl));
        struct v4l2_buffer c = {.type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1};
        int r = xioctl(e->fd, VIDIOC_DQBUF, &c);
        if (r == -EPIPE && e->stopped == 1) {  /* drained, with no buffer left to carry the LAST flag */
            e->stopped = 2;
            if (e->ops->eos) e->ops->eos(e->user);
            break;
        }
        if (r) break;
        progress++;
        if (c.index >= e->ncap) continue;
        e->cap[c.index].queued = 0;
        int last = c.flags & V4L2_BUF_FLAG_LAST;
        if (pl.bytesused > pl.data_offset && e->ops->packet) {
            const uint8_t *data = (const uint8_t *)e->cap[c.index].map + pl.data_offset;
            size_t len = pl.bytesused - pl.data_offset;
            buf_sync(e->cap[c.index].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            struct v4l2enc_packet p = {
                .data = data, .len = len,
                .timestamp = (int64_t)c.timestamp.tv_sec * 1000000 + c.timestamp.tv_usec,
                .keyframe = !!(c.flags & V4L2_BUF_FLAG_KEYFRAME), .config = !has_picture(e->codec, data, len),
                .index = c.index,
            };
            e->ops->packet(e->user, &p);
        } else {
            queue_capture(e, c.index);
        }
        if (last && e->stopped == 1) {
            e->stopped = 2;
            if (e->ops->eos) e->ops->eos(e->user);
        }
    }
    return progress;
}
