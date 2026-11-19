/* SPDX-License-Identifier: MIT
 * asteroids: a small stateful V4L2 decoder for Qualcomm's msm_video (/dev/video32): DMABUF buffers from the system
 * dma-heap (the driver has no MMAP), Annex-B H.264/HEVC in, linear NV12 out. Used by the OMX components in
 * omx_core.c; one decoder per component, driven from one thread. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum v4l2dec_codec { V4L2DEC_H264, V4L2DEC_HEVC };

struct v4l2dec_frame {
    const uint8_t *data;              /* NV12: luma, then interleaved chroma at stride * scanlines */
    unsigned width, height;           /* visible size */
    unsigned stride, scanlines;       /* plane layout */
    int64_t timestamp;                /* as queued with the input, microseconds */
    unsigned index;                   /* capture buffer, for v4l2dec_release_frame */
};

struct v4l2dec_ops {
    void (*input_done)(void *user, uint64_t cookie);       /* the input queued with this cookie was consumed */
    void (*format)(void *user, unsigned width, unsigned height, unsigned stride, unsigned scanlines);
    void (*frame)(void *user, const struct v4l2dec_frame *frame); /* call v4l2dec_release_frame when copied */
    void (*eos)(void *user);                                /* all frames before a drain request delivered */
    void (*error)(void *user, int err);
};

struct v4l2dec;

struct v4l2dec *v4l2dec_open(enum v4l2dec_codec codec, unsigned width, unsigned height,
    const struct v4l2dec_ops *ops, void *user);
void v4l2dec_close(struct v4l2dec *d);
/* Free input slots right now (bitstream buffers not held by the driver). */
int v4l2dec_input_slots(struct v4l2dec *d);
/* Queue one access unit (codec config is prepended to the next frame). 0, -EAGAIN when no slot is free. */
int v4l2dec_queue(struct v4l2dec *d, const uint8_t *data, size_t len, int64_t timestamp, uint64_t cookie);
int v4l2dec_queue_config(struct v4l2dec *d, const uint8_t *data, size_t len);
/* Ask for everything queued so far to come out, then ops->eos. */
int v4l2dec_drain(struct v4l2dec *d);
/* Drop everything queued and in flight (OMX flush); input_done is called for dropped inputs. */
int v4l2dec_flush(struct v4l2dec *d);
void v4l2dec_release_frame(struct v4l2dec *d, unsigned index);
/* Wait up to timeout_ms (0: don't wait) for the driver, then dispatch whatever it has through ops. Returns how many
 * events and buffers were handled, <0 on a fatal error. */
int v4l2dec_poll(struct v4l2dec *d, int timeout_ms);
/* The device fd, for the caller's own poll set (POLLIN | POLLOUT | POLLPRI); its flags are only a hint. */
int v4l2dec_fd(struct v4l2dec *d);
