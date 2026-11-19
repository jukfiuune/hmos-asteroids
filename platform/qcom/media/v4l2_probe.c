/* SPDX-License-Identifier: MIT
 * asteroids: lists what a V4L2 memory-to-memory node offers (Qualcomm's msm_video decoder and encoder on
 * /dev/video32 and /dev/video33): capabilities, and the formats on its OUTPUT (bitstream for a decoder) and CAPTURE
 * (frames) queues with their frame sizes. "v4l2_probe [device...]". */
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void fourcc(unsigned f, char *s) { for (int i = 0; i < 4; i++) s[i] = (char)((f >> (8 * i)) & 0xff); s[4] = 0; }

static void formats(int fd, enum v4l2_buf_type type, const char *name)
{
    for (unsigned i = 0;; i++) {
        struct v4l2_fmtdesc d = {.index = i, .type = type};
        if (ioctl(fd, VIDIOC_ENUM_FMT, &d)) break;
        char f[5]; fourcc(d.pixelformat, f);
        printf("  %s %s flags=0x%x \"%s\"", name, f, d.flags, d.description);
        struct v4l2_frmsizeenum s = {.index = 0, .pixel_format = d.pixelformat};
        if (!ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &s) && s.type != V4L2_FRMSIZE_TYPE_DISCRETE)
            printf(" %ux%u..%ux%u", s.stepwise.min_width, s.stepwise.min_height, s.stepwise.max_width,
                s.stepwise.max_height);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *def[] = {"/dev/video32", "/dev/video33"};
    int n = argc > 1 ? argc - 1 : 2;
    for (int i = 0; i < n; i++) {
        const char *dev = argc > 1 ? argv[i + 1] : def[i];
        int fd = open(dev, O_RDWR | O_NONBLOCK);
        if (fd < 0) { perror(dev); continue; }
        struct v4l2_capability cap = {0};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap)) { perror("QUERYCAP"); close(fd); continue; }
        printf("%s driver=%s card=%s bus=%s caps=0x%x device_caps=0x%x\n", dev, cap.driver, cap.card, cap.bus_info,
            cap.capabilities, cap.device_caps);
        formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT ");
        formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE");
        close(fd);
    }
    return 0;
}
