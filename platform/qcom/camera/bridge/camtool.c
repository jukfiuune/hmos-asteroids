/* SPDX-License-Identifier: MIT
 * camtool: a client for camserver (protocol.h), run as root from M1's shell, to check the camera without HarmonyOS.
 *   camtool [-s socket] list
 *   camtool [-s socket] shoot <camera> [out.jpg] [flash: off|on|torch] [frames]
 *     opens the camera with a 1920x1080 preview and its largest JPEG, runs the preview for `frames` frames (default 60),
 *     saves the last one as out.nv21 beside out.jpg, takes a still and saves it. Prints the frame rate.
 *     CAMTOOL_PREVIEW=WxH and CAMTOOL_STILL=WxH (or 0: no still stream) change the sizes, CAMTOOL_ZOOM the zoom,
 *     CAMTOOL_VIDEO=1 makes it a video session.
 * Default socket: /hmos-root/dev/unix/socket/asteroids.camera. */
#include "protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fd;
static uint32_t seq;
static uint8_t *ring;
static struct cam_layout layout;
static struct cam_event pending[64];
static int npending;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* A request and its reply; events that arrive meanwhile are kept for later. */
static int call(uint32_t op, int a0, int a1, int a2, int a3, const void *data, uint32_t bytes, void *out,
                uint32_t out_size, int *rfd)
{
    struct { struct cam_req h; uint8_t d[256]; } m = {{CAM_REQ_MAGIC, op, ++seq, bytes, {a0, a1, a2, a3}}, {0}};
    if (bytes) memcpy(m.d, data, bytes);
    if (send(fd, &m, sizeof(m.h) + bytes, 0) < 0) return -errno;
    if (op == CAM_RELEASE) return 0;
    for (;;) {
        union { struct cam_reply r; struct cam_event e; uint8_t raw[sizeof(struct cam_reply) + sizeof(struct cam_list)]; } u;
        char control[CMSG_SPACE(sizeof(int))];
        struct iovec iov = {&u, sizeof(u)};
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        ssize_t n = recvmsg(fd, &mh, 0);
        if (n <= 0) return -EPIPE;
        if (u.r.magic == CAM_EVENT_MAGIC) {
            if (npending < 64) pending[npending++] = u.e;
            continue;
        }
        if (u.r.magic != CAM_REPLY_MAGIC || u.r.seq != seq) return -EPROTO;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        if (rfd) *rfd = c && c->cmsg_type == SCM_RIGHTS ? *(int *)CMSG_DATA(c) : -1;
        if (out && u.r.bytes) memcpy(out, u.raw + sizeof(u.r), u.r.bytes < out_size ? u.r.bytes : out_size);
        return u.r.rc;
    }
}

static int event(struct cam_event *e, int timeout_ms)
{
    if (npending) {
        *e = pending[0];
        memmove(pending, pending + 1, --npending * sizeof(*e));
        return 0;
    }
    struct timeval tv = {timeout_ms / 1000, timeout_ms % 1000 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, e, sizeof(*e), 0);
    tv = (struct timeval){0, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return n == sizeof(*e) && e->magic == CAM_EVENT_MAGIC ? 0 : -1;
}

static int list(void)
{
    static struct cam_list l;
    int rc = call(CAM_LIST, 0, 0, 0, 0, NULL, 0, &l, sizeof(l), NULL);
    if (rc) return rc;
    for (uint32_t i = 0; i < l.count; i++) {
        struct cam_info *c = &l.camera[i];
        printf("camera %u (HAL %s): %s, orientation %d, flash %d, active %ux%u, focal %.2f mm, zoom %.2f-%.2f, ev %d..%d\n",
               i, c->hal_id, c->facing ? "front" : "back", c->orientation, c->flash, c->active_width, c->active_height,
               c->focal_length, c->min_zoom, c->max_zoom, c->ev_min, c->ev_max);
        printf("  preview:");
        for (uint32_t k = 0; k < c->n_preview; k++) printf(" %ux%u", c->preview[k].width, c->preview[k].height);
        printf("\n  still:");
        for (uint32_t k = 0; k < c->n_still; k++) printf(" %ux%u", c->still[k].width, c->still[k].height);
        printf("\n  fps:");
        for (uint32_t k = 0; k < c->n_fps; k++) printf(" %d-%d", c->fps[k][0], c->fps[k][1]);
        printf("\n");
    }
    return 0;
}

static int shoot(int camera, const char *out, const char *flash, int frames)
{
    static struct cam_list l;
    int rc = call(CAM_LIST, 0, 0, 0, 0, NULL, 0, &l, sizeof(l), NULL);
    if (rc || (uint32_t)camera >= l.count) return rc ? rc : -EINVAL;
    struct cam_size still = l.camera[camera].still[0], pv = {1920, 1080};
    if (getenv("CAMTOOL_PREVIEW")) sscanf(getenv("CAMTOOL_PREVIEW"), "%ux%u", &pv.width, &pv.height);
    if (getenv("CAMTOOL_STILL") && sscanf(getenv("CAMTOOL_STILL"), "%ux%u", &still.width, &still.height) != 2)
        still = (struct cam_size){0, 0};
    if ((rc = call(CAM_OPEN, camera, 0, 0, 0, NULL, 0, NULL, 0, NULL))) { printf("open: %d\n", rc); return rc; }
    struct cam_settings s = CAM_SETTINGS_DEFAULT;
    if (flash && !strcmp(flash, "on")) s.flash = CAM_FLASH_ON;
    if (flash && !strcmp(flash, "torch")) s.flash = CAM_FLASH_TORCH;
    if (getenv("CAMTOOL_ZOOM")) s.zoom = strtof(getenv("CAMTOOL_ZOOM"), NULL);
    s.video = getenv("CAMTOOL_VIDEO") != NULL;
    if ((rc = call(CAM_SETTINGS, 0, 0, 0, 0, &s, sizeof(s), NULL, 0, NULL))) { printf("settings: %d\n", rc); return rc; }
    int memfd = -1;
    rc = call(CAM_CONFIGURE, (int)pv.width, (int)pv.height, (int)still.width, (int)still.height, NULL, 0, &layout, sizeof(layout), &memfd);
    if (rc || memfd < 0) { printf("configure: %d\n", rc); return rc ? rc : -EIO; }
    ring = mmap(NULL, layout.total, PROT_READ, MAP_SHARED, memfd, 0);
    if (ring == MAP_FAILED) return -errno;
    if ((rc = call(CAM_START, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL))) { printf("start: %d\n", rc); return rc; }
    double t0 = now(), t1 = t0;
    int got = 0;
    struct cam_event e;
    while (got < frames && !event(&e, 3000)) {
        if (e.type != CAM_EV_FRAME) continue;
        if (++got == frames) {
            char path[256];
            snprintf(path, sizeof(path), "%.*s.nv21", (int)(strrchr(out, '.') ? strrchr(out, '.') - out : (long)strlen(out)), out);
            FILE *f = fopen(path, "w");
            if (f) { fwrite(ring + (size_t)e.slot * layout.slot_size, 1, layout.slot_size, f); fclose(f); }
        }
        if (got == 1) t0 = now();
        t1 = now();
        call(CAM_RELEASE, (int)e.slot, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    }
    printf("preview: %d frames, %.1f fps\n", got, got > 1 ? (got - 1) / (t1 - t0) : 0);
    if (got < frames) return -ETIMEDOUT;
    if (!still.width) goto done;
    if ((rc = call(CAM_STILL, 7, 0, 0, 0, NULL, 0, NULL, 0, NULL))) { printf("still: %d\n", rc); return rc; }
    double ts = now();
    int shutter = 0;
    while (!event(&e, 8000)) {
        if (e.type == CAM_EV_FRAME) { call(CAM_RELEASE, (int)e.slot, 0, 0, 0, NULL, 0, NULL, 0, NULL); continue; }
        if (e.type == CAM_EV_SHUTTER) { shutter = 1; continue; }
        if (e.type != CAM_EV_STILL) continue;
        printf("still %u: status %d, %u bytes, shutter %d, %.2f s, iso %d, exposure %.1f ms\n", e.id, e.status, e.size,
               shutter, now() - ts, e.iso, e.exposure_ns / 1e6);
        if (!e.status) {
            FILE *f = fopen(out, "w");
            if (f) { fwrite(ring + layout.jpeg_offset, 1, e.size, f); fclose(f); }
        }
        break;
    }
done:
    call(CAM_STOP, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    return call(CAM_CLOSE, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL);
}

int main(int argc, char **argv)
{
    const char *path = "/hmos-root/dev/unix/socket/" CAM_SOCKET;
    if (argc > 2 && !strcmp(argv[1], "-s")) { path = argv[2]; argv += 2; argc -= 2; }
    if (argc < 2) { fprintf(stderr, "camtool [-s socket] list | shoot <camera> [out.jpg] [off|on|torch] [frames]\n"); return 2; }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { perror(path); return 1; }
    int rc = -EINVAL;
    if (!strcmp(argv[1], "list")) rc = list();
    else if (!strcmp(argv[1], "shoot") && argc > 2)
        rc = shoot(atoi(argv[2]), argc > 3 ? argv[3] : "/data/local/tmp/camtool.jpg", argc > 4 ? argv[4] : "off",
                   argc > 5 ? atoi(argv[5]) : 60);
    printf("rc %d\n", rc);
    return rc ? 1 : 0;
}
