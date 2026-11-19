/* SPDX-License-Identifier: MIT
 * asteroids: a small stateful V4L2 encoder for Qualcomm's msm_video (/dev/video33): DMABUF buffers from the system
 * dma-heap (the driver has no MMAP), linear NV12 or NV21 in, Annex-B H.264/HEVC out. Used by the OMX encoder
 * components in omx_core.c; one encoder per component, driven from one thread. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum v4l2enc_codec { V4L2ENC_H264, V4L2ENC_HEVC };

struct v4l2enc_config {
    enum v4l2enc_codec codec;
    unsigned width, height;
    int nv21;                         /* input chroma order: V first */
    unsigned fps_q16;                 /* frames per second, Q16 */
    unsigned bitrate;                 /* bits per second, at most (the driver's CBR) */
    unsigned gop;                     /* frames from one key frame to the next; 0: the driver's choice */
};

struct v4l2enc_packet {
    const uint8_t *data;
    size_t len;
    int64_t timestamp;                /* as queued with the frame, microseconds */
    int keyframe, config;             /* a sync frame; only parameter sets (VPS/SPS/PPS) */
    unsigned index;                   /* capture buffer, for v4l2enc_release_packet */
};

struct v4l2enc_ops {
    void (*input_done)(void *user, uint64_t cookie);       /* the frame queued with this cookie was consumed */
    void (*packet)(void *user, const struct v4l2enc_packet *packet); /* call v4l2enc_release_packet when copied */
    void (*eos)(void *user);                                /* all packets before a drain request delivered */
    void (*error)(void *user, int err);
    void (*log)(void *user, const char *msg);             /* optional */
};

struct v4l2enc;

struct v4l2enc *v4l2enc_open(const struct v4l2enc_config *cfg, const struct v4l2enc_ops *ops, void *user);
void v4l2enc_close(struct v4l2enc *e);
/* The layout frames are written in: luma rows of stride bytes, chroma from stride * scanlines. */
void v4l2enc_layout(struct v4l2enc *e, unsigned *stride, unsigned *scanlines);
/* A free frame buffer to write into (its index, or -EAGAIN), then queue it. */
int v4l2enc_get_input(struct v4l2enc *e, uint8_t **data);
int v4l2enc_queue_input(struct v4l2enc *e, int index, int64_t timestamp, uint64_t cookie);
int v4l2enc_request_keyframe(struct v4l2enc *e);
int v4l2enc_set_bitrate(struct v4l2enc *e, unsigned bitrate);
/* Ask for everything queued so far to come out, then ops->eos. */
int v4l2enc_drain(struct v4l2enc *e);
void v4l2enc_release_packet(struct v4l2enc *e, unsigned index);
/* Dispatch whatever the driver has through ops (doesn't wait). Returns how many buffers were handled, <0 on a fatal
 * error. */
int v4l2enc_poll(struct v4l2enc *e);
int v4l2enc_fd(struct v4l2enc *e);
