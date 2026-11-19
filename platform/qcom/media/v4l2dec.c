/* SPDX-License-Identifier: MIT
 * asteroids: stateful V4L2 decoding on Qualcomm's msm_video, see v4l2dec.h. */
#include "v4l2dec.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define NOUT 8
#define MAXCAP 32

struct slot { int fd; void *map; size_t len; int queued; uint64_t cookie; };

struct v4l2dec {
    int fd, heap;
    const struct v4l2dec_ops *ops;
    void *user;
    struct slot in[NOUT];
    unsigned nin;
    struct slot cap[MAXCAP];
    unsigned ncap, width, height, stride, scanlines, visible_w, visible_h;
    int capture_on, stopped;          /* stopped: 1 draining, 2 drained */
    uint8_t *config;
    size_t config_len;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : 0;
}

static int alloc_slot(struct v4l2dec *d, struct slot *s, size_t len)
{
    struct dma_heap_allocation_data a = {.len = len, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(d->heap, DMA_HEAP_IOCTL_ALLOC, &a)) return -errno;
    void *map = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
    if (map == MAP_FAILED) { int e = -errno; close(a.fd); return e; }
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

static int queue_capture(struct v4l2dec *d, unsigned i)
{
    struct v4l2_plane pl = {.m.fd = d->cap[i].fd, .length = d->cap[i].len};
    struct v4l2_buffer b = {.type = CAP, .memory = V4L2_MEMORY_DMABUF, .index = i, .m.planes = &pl, .length = 1};
    int r = xioctl(d->fd, VIDIOC_QBUF, &b);
    if (!r) d->cap[i].queued = 1;
    return r;
}

static void stop_capture(struct v4l2dec *d)
{
    if (!d->capture_on) return;
    int t = CAP;
    xioctl(d->fd, VIDIOC_STREAMOFF, &t);
    struct v4l2_requestbuffers rb = {.count = 0, .type = CAP, .memory = V4L2_MEMORY_DMABUF};
    xioctl(d->fd, VIDIOC_REQBUFS, &rb);
    for (unsigned i = 0; i < d->ncap; i++) free_slot(&d->cap[i]);
    d->ncap = 0; d->capture_on = 0;
}

/* After a source change: the capture queue at the new size, linear NV12. */
static int start_capture(struct v4l2dec *d)
{
    stop_capture(d);
    struct v4l2_format f = {.type = CAP};
    int r = xioctl(d->fd, VIDIOC_G_FMT, &f);
    if (r) return r;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if ((r = xioctl(d->fd, VIDIOC_S_FMT, &f))) return r;
    d->width = f.fmt.pix_mp.width; d->height = f.fmt.pix_mp.height;
    d->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    /* chroma follows the coded (aligned) height of luma rows; sizeimage also covers the chroma and more */
    d->scanlines = d->height;
    struct v4l2_selection sel = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .target = V4L2_SEL_TGT_COMPOSE};
    d->visible_w = d->width; d->visible_h = d->height;
    if (!xioctl(d->fd, VIDIOC_G_SELECTION, &sel) && sel.r.width && sel.r.height) {
        d->visible_w = sel.r.width; d->visible_h = sel.r.height;
    }
    struct v4l2_control min = {.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
    unsigned want = 6;
    if (!xioctl(d->fd, VIDIOC_G_CTRL, &min)) want = min.value + 4;
    if (want > MAXCAP) want = MAXCAP;
    struct v4l2_requestbuffers rb = {.count = want, .type = CAP, .memory = V4L2_MEMORY_DMABUF};
    if ((r = xioctl(d->fd, VIDIOC_REQBUFS, &rb))) return r;
    d->ncap = rb.count > MAXCAP ? MAXCAP : rb.count;
    for (unsigned i = 0; i < d->ncap; i++) {
        if ((r = alloc_slot(d, &d->cap[i], f.fmt.pix_mp.plane_fmt[0].sizeimage))) return r;
        if ((r = queue_capture(d, i))) return r;
    }
    int t = CAP;
    if ((r = xioctl(d->fd, VIDIOC_STREAMON, &t))) return r;
    d->capture_on = 1;
    if (d->ops->format) d->ops->format(d->user, d->visible_w, d->visible_h, d->stride, d->scanlines);
    return 0;
}

struct v4l2dec *v4l2dec_open(enum v4l2dec_codec codec, unsigned width, unsigned height,
    const struct v4l2dec_ops *ops, void *user)
{
    struct v4l2dec *d = calloc(1, sizeof(*d));
    if (!d) return 0;
    d->ops = ops; d->user = user;
    d->heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    d->fd = open("/dev/video32", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d->heap < 0 || d->fd < 0) goto fail;
    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_SOURCE_CHANGE};
    xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = V4L2_EVENT_EOS;
    xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    struct v4l2_format f = {.type = OUT};
    f.fmt.pix_mp.pixelformat = codec == V4L2DEC_HEVC ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.width = width ? width : 1920;
    f.fmt.pix_mp.height = height ? height : 1080;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f) || xioctl(d->fd, VIDIOC_G_FMT, &f)) goto fail;
    struct v4l2_requestbuffers rb = {.count = NOUT, .type = OUT, .memory = V4L2_MEMORY_DMABUF};
    if (xioctl(d->fd, VIDIOC_REQBUFS, &rb)) goto fail;
    d->nin = rb.count < NOUT ? rb.count : NOUT;
    for (unsigned i = 0; i < d->nin; i++)
        if (alloc_slot(d, &d->in[i], f.fmt.pix_mp.plane_fmt[0].sizeimage)) goto fail;
    int t = OUT;
    if (xioctl(d->fd, VIDIOC_STREAMON, &t)) goto fail;
    return d;
fail:
    v4l2dec_close(d);
    return 0;
}

void v4l2dec_close(struct v4l2dec *d)
{
    if (!d) return;
    if (d->fd >= 0) {
        int t = OUT;
        xioctl(d->fd, VIDIOC_STREAMOFF, &t);
        stop_capture(d);
        struct v4l2_requestbuffers rb = {.count = 0, .type = OUT, .memory = V4L2_MEMORY_DMABUF};
        xioctl(d->fd, VIDIOC_REQBUFS, &rb);
    }
    for (unsigned i = 0; i < d->nin; i++) free_slot(&d->in[i]);
    if (d->fd >= 0) close(d->fd);
    if (d->heap >= 0) close(d->heap);
    free(d->config);
    free(d);
}

int v4l2dec_fd(struct v4l2dec *d) { return d->fd; }

int v4l2dec_input_slots(struct v4l2dec *d)
{
    int n = 0;
    for (unsigned i = 0; i < d->nin; i++) n += !d->in[i].queued;
    return n;
}

int v4l2dec_queue_config(struct v4l2dec *d, const uint8_t *data, size_t len)
{
    uint8_t *c = realloc(d->config, d->config_len + len);
    if (!c) return -ENOMEM;
    memcpy(c + d->config_len, data, len);
    d->config = c; d->config_len += len;
    return 0;
}

int v4l2dec_queue(struct v4l2dec *d, const uint8_t *data, size_t len, int64_t timestamp, uint64_t cookie)
{
    unsigned i;
    for (i = 0; i < d->nin && d->in[i].queued; i++) {}
    if (i == d->nin) return -EAGAIN;
    struct slot *s = &d->in[i];
    if (d->config_len + len > s->len) return -E2BIG;
    if (d->stopped) {
        struct v4l2_decoder_cmd cmd = {.cmd = V4L2_DEC_CMD_START};
        xioctl(d->fd, VIDIOC_DECODER_CMD, &cmd);
        d->stopped = 0;
    }
    buf_sync(s->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    memcpy(s->map, d->config, d->config_len);
    memcpy((uint8_t *)s->map + d->config_len, data, len);
    buf_sync(s->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    struct v4l2_plane pl = {.m.fd = s->fd, .length = s->len, .bytesused = d->config_len + len};
    struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_DMABUF, .index = i, .m.planes = &pl, .length = 1};
    b.timestamp.tv_sec = timestamp / 1000000;
    b.timestamp.tv_usec = timestamp % 1000000;
    int r = xioctl(d->fd, VIDIOC_QBUF, &b);
    if (r) return r;
    free(d->config); d->config = 0; d->config_len = 0;
    s->queued = 1; s->cookie = cookie;
    return 0;
}

int v4l2dec_drain(struct v4l2dec *d)
{
    struct v4l2_decoder_cmd cmd = {.cmd = V4L2_DEC_CMD_STOP};
    int r = xioctl(d->fd, VIDIOC_DECODER_CMD, &cmd);
    if (!r) d->stopped = 1;
    return r;
}

int v4l2dec_flush(struct v4l2dec *d)
{
    int t = OUT;
    xioctl(d->fd, VIDIOC_STREAMOFF, &t);
    for (unsigned i = 0; i < d->nin; i++)
        if (d->in[i].queued) {
            d->in[i].queued = 0;
            if (d->ops->input_done) d->ops->input_done(d->user, d->in[i].cookie);
        }
    if (d->capture_on) {
        t = CAP;
        xioctl(d->fd, VIDIOC_STREAMOFF, &t);
        for (unsigned i = 0; i < d->ncap; i++) d->cap[i].queued = 0;
        for (unsigned i = 0; i < d->ncap; i++) queue_capture(d, i);
        xioctl(d->fd, VIDIOC_STREAMON, &t);
    }
    t = OUT;
    d->stopped = 0;
    return xioctl(d->fd, VIDIOC_STREAMON, &t);
}

void v4l2dec_release_frame(struct v4l2dec *d, unsigned index)
{
    if (index >= d->ncap || d->cap[index].queued || !d->capture_on) return;
    buf_sync(d->cap[index].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    queue_capture(d, index);
}

int v4l2dec_poll(struct v4l2dec *d, int timeout_ms)
{
    /* msm_vidc's poll flags (POLLERR while a queue is idle among them) only say "look": the fd is non-blocking, so
     * every queue is simply tried. */
    struct pollfd p = {d->fd, POLLIN | POLLOUT | POLLPRI, 0};
    if (timeout_ms && poll(&p, 1, timeout_ms) < 0 && errno != EINTR) return -errno;
    int progress = 0;
    struct v4l2_event ev;
    while (!xioctl(d->fd, VIDIOC_DQEVENT, &ev)) {
        progress++;
        /* V4L2_EVENT_EOS can come before the last frames are dequeued: the LAST capture buffer ends a drain. */
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
            int r = start_capture(d);
            if (r) { if (d->ops->error) d->ops->error(d->user, r); return r; }
        }
    }
    struct v4l2_plane pl = {0};
    struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1};
    while (!xioctl(d->fd, VIDIOC_DQBUF, &b)) {
        progress++;
        if (b.index < d->nin && d->in[b.index].queued) {
            d->in[b.index].queued = 0;
            if (d->ops->input_done) d->ops->input_done(d->user, d->in[b.index].cookie);
        }
    }
    while (d->capture_on) {
        memset(&pl, 0, sizeof(pl));
        struct v4l2_buffer c = {.type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1};
        if (xioctl(d->fd, VIDIOC_DQBUF, &c)) break;
        progress++;
        if (c.index >= d->ncap) continue;
        d->cap[c.index].queued = 0;
        int last = c.flags & V4L2_BUF_FLAG_LAST;
        if (pl.bytesused && d->ops->frame) {
            struct v4l2dec_frame f = {
                .data = d->cap[c.index].map, .width = d->visible_w, .height = d->visible_h,
                .stride = d->stride, .scanlines = d->scanlines,
                .timestamp = (int64_t)c.timestamp.tv_sec * 1000000 + c.timestamp.tv_usec, .index = c.index,
            };
            buf_sync(d->cap[c.index].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            d->ops->frame(d->user, &f);
        } else {
            queue_capture(d, c.index);
        }
        if (last && d->stopped == 1 && d->ops->eos) { d->stopped = 2; d->ops->eos(d->user); }
    }
    return progress;
}
