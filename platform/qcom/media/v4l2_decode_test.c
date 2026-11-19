/* SPDX-License-Identifier: MIT
 * asteroids: decodes an H.264 or HEVC Annex-B elementary stream on Qualcomm's msm_video decoder (/dev/video32,
 * V4L2 stateful memory-to-memory, DMABUF buffers from /dev/dma_heap/system (the driver has no MMAP), linear NV12 out) and reports frames and speed; writes frame N as
 * a PGM of its luma plane: "v4l2_decode_test <stream.h264|.hevc> [out.pgm] [frame]". */
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define NOUT 8
#define MAXCAP 32

static int fd, heap;
static int outfd[NOUT], capfd[MAXCAP];

/* A dma-buf of n bytes from the system heap, mapped. */
static int alloc_buf(size_t n, void **map)
{
    struct dma_heap_allocation_data a = {.len = n, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a)) { perror("DMA_HEAP_IOCTL_ALLOC"); return -1; }
    *map = mmap(0, n, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
    if (*map == MAP_FAILED) { perror("mmap dma-buf"); return -1; }
    return a.fd;
}
static unsigned char *data;
static size_t size, pos;
static void *outmap[NOUT];
static size_t outlen[NOUT];
static void *capmap[MAXCAP][2];
static size_t caplen[MAXCAP][2];
static unsigned ncap, nout, width, height, stride, scanlines;
static int hevc;

static int xioctl(unsigned long req, void *arg, const char *name)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    if (r < 0 && errno != EAGAIN) fprintf(stderr, "%s: %s\n", name, strerror(errno));
    return r;
}

/* The next access unit: from one start code to the next frame's first VCL NAL (H.264 slices 1/5 with
 * first_mb_in_slice 0, HEVC VCL 0..31 with first_slice_segment_in_pic_flag). Parameter sets go with their frame. */
static size_t next_start(size_t p)
{
    for (; p + 3 < size; p++)
        if (data[p] == 0 && data[p + 1] == 0 && (data[p + 2] == 1 || (data[p + 2] == 0 && data[p + 3] == 1))) return p;
    return size;
}
static size_t nal_payload(size_t p) { return data[p + 2] == 1 ? p + 3 : p + 4; }
static int starts_frame(size_t p)
{
    size_t n = nal_payload(p);
    if (n + 2 >= size) return 0;
    if (hevc) { int t = (data[n] >> 1) & 0x3f; return t <= 31 && (data[n + 2] & 0x80); }
    int t = data[n] & 0x1f;
    return (t == 1 || t == 5) && (data[n + 1] & 0x80);
}
static size_t access_unit(size_t *start)
{
    size_t p = next_start(pos), seen_vcl = 0;
    if (p >= size) return 0;
    *start = p;
    for (size_t q = p; q < size; q = next_start(q + 3)) {
        if (starts_frame(q)) { if (seen_vcl) { pos = q; return q - p; } seen_vcl = 1; }
        if (q + 3 >= size) break;
    }
    pos = size;
    return size - p;
}

static int queue_output(unsigned index)
{
    size_t start = 0, n = access_unit(&start);
    struct v4l2_plane pl = {0};
    struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_DMABUF, .index = index, .m.planes = &pl, .length = 1};
    pl.m.fd = outfd[index]; pl.length = outlen[index];
    if (n > outlen[index]) n = outlen[index];
    if (n) memcpy(outmap[index], data + start, n);
    else b.flags = V4L2_BUF_FLAG_LAST;
    pl.bytesused = n ? n : 0;
    if (!n) return 1;
    return xioctl(VIDIOC_QBUF, &b, "QBUF out") < 0 ? -1 : 0;
}

static int setup_capture(void)
{
    struct v4l2_format f = {.type = CAP};
    if (xioctl(VIDIOC_G_FMT, &f, "G_FMT cap")) return -1;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(VIDIOC_S_FMT, &f, "S_FMT cap")) return -1;
    width = f.fmt.pix_mp.width; height = f.fmt.pix_mp.height;
    stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    scanlines = f.fmt.pix_mp.plane_fmt[0].sizeimage / (stride ? stride : 1);
    printf("V4L2_CAPTURE %ux%u stride=%u planes=%u size0=%u\n", width, height, stride, f.fmt.pix_mp.num_planes,
        f.fmt.pix_mp.plane_fmt[0].sizeimage);
    struct v4l2_control min = {.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
    unsigned want = 4;
    if (!ioctl(fd, VIDIOC_G_CTRL, &min)) want = min.value + 4;
    if (want > MAXCAP) want = MAXCAP;
    struct v4l2_requestbuffers rb = {.count = want, .type = CAP, .memory = V4L2_MEMORY_DMABUF};
    if (xioctl(VIDIOC_REQBUFS, &rb, "REQBUFS cap")) return -1;
    ncap = rb.count;
    for (unsigned i = 0; i < ncap; i++) {
        caplen[i][0] = f.fmt.pix_mp.plane_fmt[0].sizeimage;
        capfd[i] = alloc_buf(caplen[i][0], &capmap[i][0]);
        if (capfd[i] < 0) return -1;
        struct v4l2_plane pl[1] = {{.m.fd = capfd[i], .length = caplen[i][0]}};
        struct v4l2_buffer b = {.type = CAP, .memory = V4L2_MEMORY_DMABUF, .index = i, .m.planes = pl, .length = 1};
        if (xioctl(VIDIOC_QBUF, &b, "QBUF cap")) return -1;
    }
    int t = CAP;
    return xioctl(VIDIOC_STREAMON, &t, "STREAMON cap");
}

int main(int argc, char **argv)
{
    setbuf(stdout, 0);
    if (argc < 2) return 1;
    int in = open(argv[1], O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st)) { perror(argv[1]); return 1; }
    size = st.st_size;
    data = mmap(0, size, PROT_READ, MAP_PRIVATE, in, 0);
    hevc = strstr(argv[1], "hevc") || strstr(argv[1], "265");
    long dump = argc > 3 ? atol(argv[3]) : 30;
    heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0) { perror("/dev/dma_heap/system"); return 2; }
    fd = open("/dev/video32", O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("/dev/video32"); return 2; }
    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_SOURCE_CHANGE};
    xioctl(VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE source");
    sub.type = V4L2_EVENT_EOS;
    xioctl(VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE eos");
    struct v4l2_format f = {.type = OUT};
    f.fmt.pix_mp.pixelformat = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.width = 1920; f.fmt.pix_mp.height = 1080; f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 2 << 20;
    if (xioctl(VIDIOC_S_FMT, &f, "S_FMT out")) return 3;
    if (xioctl(VIDIOC_G_FMT, &f, "G_FMT out")) return 3;
    struct v4l2_requestbuffers rb = {.count = NOUT, .type = OUT, .memory = V4L2_MEMORY_DMABUF};
    if (xioctl(VIDIOC_REQBUFS, &rb, "REQBUFS out")) return 3;
    nout = rb.count < NOUT ? rb.count : NOUT;
    printf("V4L2_OUTPUT_BUFFERS %u size=%u\n", nout, f.fmt.pix_mp.plane_fmt[0].sizeimage);
    for (unsigned i = 0; i < nout; i++) {
        outlen[i] = f.fmt.pix_mp.plane_fmt[0].sizeimage;
        outfd[i] = alloc_buf(outlen[i], &outmap[i]);
        if (outfd[i] < 0) return 3;
    }
    int t = OUT;
    if (xioctl(VIDIOC_STREAMON, &t, "STREAMON out")) return 3;
    unsigned queued = 0;
    for (unsigned i = 0; i < nout; i++) if (!queue_output(i)) queued++;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long frames = 0, sent = queued;
    int capture_on = 0, drained = 0, stopped = 0;
    while (!drained) {
        struct pollfd p = {fd, POLLIN | POLLOUT | POLLPRI, 0};
        if (poll(&p, 1, 3000) <= 0) { puts("V4L2_TIMEOUT"); break; }
        if (p.revents & POLLPRI) {
            struct v4l2_event ev;
            while (!ioctl(fd, VIDIOC_DQEVENT, &ev)) {
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !capture_on) {
                    if (setup_capture()) return 4;
                    capture_on = 1;
                } else if (ev.type == V4L2_EVENT_EOS) drained = 1;
            }
        }
        if (p.revents & POLLOUT) {
            struct v4l2_plane pl = {0};
            struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1};
            while (!ioctl(fd, VIDIOC_DQBUF, &b)) {
                int r = stopped ? 1 : queue_output(b.index);
                if (!r) sent++;
                else if (r > 0 && !stopped) {
                    struct v4l2_decoder_cmd cmd = {.cmd = V4L2_DEC_CMD_STOP};
                    xioctl(VIDIOC_DECODER_CMD, &cmd, "DECODER_CMD stop");
                    stopped = 1;
                }
            }
        }
        if (capture_on && (p.revents & POLLIN)) {
            struct v4l2_plane pl[1] = {0};
            struct v4l2_buffer b = {.type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = pl, .length = 1};
            while (!ioctl(fd, VIDIOC_DQBUF, &b)) {
                if (pl[0].bytesused) {
                    if (frames == dump && argc > 2) {
                        FILE *o = fopen(argv[2], "wb");
                        fprintf(o, "P5\n%u %u\n255\n", width, height);
                        for (unsigned y = 0; y < height; y++) fwrite((char *)capmap[b.index][0] + y * stride, 1, width, o);
                        fclose(o);
                    }
                    frames++;
                }
                if (b.flags & V4L2_BUF_FLAG_LAST) drained = 1;
                pl[0].m.fd = capfd[b.index]; pl[0].length = caplen[b.index][0]; pl[0].bytesused = 0;
                xioctl(VIDIOC_QBUF, &b, "QBUF cap");
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("V4L2_RESULT sent=%ld frames=%ld ms=%.0f fps=%.1f\n", sent, frames, ms, ms > 0 ? frames * 1000.0 / ms : 0);
    return frames > 0 ? 0 : 5;
}
