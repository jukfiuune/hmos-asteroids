// drmtest: lights the panel through DRM/KMS with a dumb buffer (no GPU), to prove the display path on its own.
// Picks the DSI connector (the panel) and its preferred mode, draws color bars with a white border and an "up"
// arrow (so the camera shows which way is up), sets the CRTC and holds it for the given number of seconds.
//     drmtest [seconds] [/dev/dri/card0]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static void draw(uint32_t *px, uint32_t w, uint32_t h, uint32_t stride)
{
    static const uint32_t bars[] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = px + y * (stride / 4);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = bars[x * 8 / w];
            if (x < 16 || y < 16 || x >= w - 16 || y >= h - 16)
                c = 0xffffff;
            // An arrow pointing to the top of the panel, in the upper third.
            int32_t cx = (int32_t)w / 2, dy = (int32_t)y - (int32_t)h / 8, dx = abs((int32_t)x - cx);
            if (dy >= 0 && dy < (int32_t)h / 6 && dx < dy / 2 + 1)
                c = 0x202020;
            if (dy >= (int32_t)h / 12 && dy < (int32_t)h / 4 && dx < (int32_t)w / 20)
                c = 0x202020;
            row[x] = 0xff000000 | c;
        }
    }
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    const char *path = argc > 2 ? argv[2] : "/dev/dri/card0";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(path); return 1; }
    if (drmSetMaster(fd)) perror("drmSetMaster (continuing)");

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); return 1; }
    // The panel is the DSI connector; msm_drm also reports a "connected" virtual one (writeback, 5120x2560).
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        printf("connector %u type %u status %d modes %d\n", c->connector_id, c->connector_type, c->connection,
               c->count_modes);
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
            (!conn || c->connector_type == DRM_MODE_CONNECTOR_DSI))
            conn = c;
    }
    if (!conn) { fprintf(stderr, "no connected connector\n"); return 1; }
    drmModeModeInfo mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) mode = conn->modes[i];
    printf("mode %s %ux%u@%u\n", mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);

    uint32_t crtc = 0;
    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id) crtc = enc->crtc_id;
    for (int e = 0; !crtc && e < conn->count_encoders; e++) {
        drmModeEncoder *en = drmModeGetEncoder(fd, conn->encoders[e]);
        for (int i = 0; en && i < res->count_crtcs; i++)
            if (en->possible_crtcs & (1u << i)) { crtc = res->crtcs[i]; break; }
    }
    printf("crtc %u\n", crtc);

    struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("CREATE_DUMB"); return 1; }
    uint32_t fb;
    if (drmModeAddFB(fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle, &fb)) { perror("AddFB"); return 1; }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("MAP_DUMB"); return 1; }
    uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (px == MAP_FAILED) { perror("mmap"); return 1; }
    draw(px, cd.width, cd.height, cd.pitch);

    if (drmModeSetCrtc(fd, crtc, fb, 0, 0, &conn->connector_id, 1, &mode)) { perror("SetCrtc"); return 1; }
    printf("showing for %d s\n", secs);
    fflush(stdout);
    sleep(secs);
    return 0;
}
