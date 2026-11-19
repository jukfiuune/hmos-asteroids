/* camserver: Qualcomm's camera HAL (CamX) behind a socket, for HarmonyOS's camera host (camera_host.cpp); the protocol
 * is protocol.h. Runs in the camera root (camera-root.sh) as a preload of a vendor program, as the probe does
 * (camx_probe.c): its constructor starts a thread that does the work while the host program (sleep) waits, because
 * CamX's worker threads dlopen while the linker holds its lock during constructors.
 *   CAMSERVER_SOCKET   where to listen (default /hmos-socket/asteroids.camera: HarmonyOS's /dev/unix/socket)
 *   CAMSERVER_UID      the uid that may connect, besides root, and the socket's owner (HarmonyOS's camera_host)
 *   CAMSERVER_CAMERAS  the HAL's camera ids to offer, in order (default "0,1": main back and front); "0+2+3" is one
 *                      camera with lenses 0, 2 and 3, the first its 1x: it zooms across them, handing over to the
 *                      narrowest lens that holds the zoom (camera.sh: the main, ultrawide and tele lenses). The HAL's
 *                      own logical camera (4) would do that without a gap, but fails every request (its main lens's
 *                      realtime pipeline doesn't finalize, with Android's full images too).
 * One camera open at a time. The preview is the HAL's YCbCr_420_888 stream (a camera app's preview usage: GPU texture
 * and composer, which keeps it uncompressed; implementation-defined would be UBWC), copied into the shared memory slots
 * as NV21. The still is the HAL's JPEG (BLOB) stream, added to one request. CHI's own buffer manager is off
 * (halBufferMgrMode=0 in camxoverridesettings.txt): every request carries its buffers.
 * The compute DSP runs only while a camera is open: CamX's motion estimation (libhme) needs it, and the boot leaves it
 * off. */
#define HAL3_FN static
#include "../hal3.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* Built with musl's headers against bionic: bionic's errno. */
int *__errno(void);
#undef errno
#define errno (*__errno())

#define LOG(...) dprintf(1, "CAMSERVER " __VA_ARGS__)
#define NPREVIEW 8         /* preview buffers the HAL writes into */
#define NSLOTS 4           /* shared slots the host reads from */
#define MAX_IN_FLIGHT 4
#define PREFLASH_FRAMES 10 /* frames with the flash lit before a flash still, for exposure to settle */
/* The YCbCr_420_888 buffers gralloc gives here are described as NV21 (Cr first) but hold U first: read as described,
 * red came out blue. */
#define CHROMA_U_FIRST 1

static camera_module_t *module;
static struct cam_list list;
/* An offered camera is one HAL camera or a group of them, its lenses: the back camera's main, ultrawide and tele.
 * `ratio` is a lens's zoom against the first's (by their fields of view), max_zoom its own digital zoom. */
#define MAX_LENSES 4
struct lens { int hal; const void *meta; float ratio, max_zoom; };
static struct lens lenses[CAM_MAX_CAMERAS][MAX_LENSES];
static int n_lenses[CAM_MAX_CAMERAS];

struct buf { AHardwareBuffer *ahb; buffer_handle_t handle; int busy; };

/* The open camera. `lock` guards everything below it; results arrive on the HAL's threads. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int client = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static camera3_device_t *dev;
static int cam = -1, lens, configured, streaming, stopping, in_flight;  /* the open camera and lens */
static camera3_stream_t preview_stream, jpeg_stream;
static int has_jpeg;
static struct buf preview[NPREVIEW], jpeg;
static uint32_t jpeg_capacity;      /* the open lens's JPEG buffer; the ring's JPEG slot holds the largest */
static uint32_t still_w, still_h;   /* the still the host asked for (the first lens's size) */
static uint8_t *ring;
static struct cam_layout layout;
static int ring_fd = -1;
static int slot_lent[NSLOTS];
static struct cam_settings settings = CAM_SETTINGS_DEFAULT;
static unsigned settings_gen;
static void *preview_md, *still_md;
static unsigned md_gen = ~0u;
static uint32_t frame_no;
/* A still asked for: preflash frames left, then its frame number while in flight. */
static int still_pending, still_preflash, still_flash;
static uint32_t still_id, still_frame = ~0u;
static int32_t last_iso;
static uint64_t shutter_ts[32];  /* each frame's start of exposure, by frame number */
static int64_t last_exposure;
/* The zoom the requests carry, which moves from zoom_from to settings.zoom over settings.zoom_ms (a smooth zoom: a
 * logical camera hands over between its lenses on the way); md_zoom is preview_md's. */
static float zoom_now = 1, zoom_from = 1, md_zoom;
static struct timespec zoom_t0;
/* Focus triggers for the next request: a scan at the focus point, or back to continuous focus. */
static int af_start, af_cancel;
static int lens_failed[MAX_LENSES];  /* lenses that wouldn't take the streams: not tried again while open */
static int last_af = -1, last_ae = -1;

static void send_event(struct cam_event *e)
{
    e->magic = CAM_EVENT_MAGIC;
    pthread_mutex_lock(&send_lock);
    int fd = client;
    ssize_t n = fd >= 0 ? send(fd, e, sizeof(*e), MSG_DONTWAIT | MSG_NOSIGNAL) : -1;
    pthread_mutex_unlock(&send_lock);
    if (n != (ssize_t)sizeof(*e) && e->type == CAM_EV_FRAME) {
        pthread_mutex_lock(&lock);
        slot_lent[e->slot] = 0;
        pthread_mutex_unlock(&lock);
    }
}

static int reply(uint32_t seq, int rc, const void *data, uint32_t bytes, int fd)
{
    struct cam_reply r = {CAM_REPLY_MAGIC, seq, rc, bytes};
    struct iovec iov[2] = {{&r, sizeof(r)}, {(void *)data, bytes}};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr m = {.msg_iov = iov, .msg_iovlen = bytes ? 2 : 1};
    if (fd >= 0) {
        m.msg_control = control;
        m.msg_controllen = sizeof(control);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    pthread_mutex_lock(&send_lock);
    ssize_t n = sendmsg(client, &m, MSG_NOSIGNAL);
    pthread_mutex_unlock(&send_lock);
    return n == (ssize_t)(sizeof(r) + bytes) ? 0 : -1;
}

/* --- HAL callbacks --- */

static struct buf *ours(const camera3_stream_buffer_t *b)
{
    if (!b->buffer) return NULL;
    if (b->buffer == &jpeg.handle || *b->buffer == jpeg.handle) return &jpeg;
    for (int k = 0; k < NPREVIEW; k++)
        if (b->buffer == &preview[k].handle || *b->buffer == preview[k].handle) return &preview[k];
    return NULL;
}

static void wait_fence(int fd)
{
    if (fd < 0) return;
    struct pollfd p = {fd, POLLIN, 0};
    poll(&p, 1, 3000);
    close(fd);
}

/* A preview frame into a free slot, as NV21; with lock held. Returns the slot or -1. */
static int copy_preview(struct buf *b)
{
    int slot = -1;
    for (int i = 0; i < NSLOTS && slot < 0; i++)
        if (!slot_lent[i]) slot = i;
    if (slot < 0) return -1;
    AHB_Planes p;
    if (ahb_lock_planes(b->ahb, CPU_READ, -1, NULL, &p) || p.count < 3) return -1;
    uint32_t w = layout.width, h = layout.height;
    uint8_t *out = ring + (size_t)slot * layout.slot_size;
    for (uint32_t y = 0; y < h; y++) memcpy(out + (size_t)y * w, p.planes[0].data + (size_t)y * p.planes[0].row_stride, w);
    uint8_t *vu = out + (size_t)w * h;
    const uint8_t *c = p.planes[1].data < p.planes[2].data ? p.planes[1].data : p.planes[2].data;
    uint32_t cs = p.planes[1].row_stride;
    if (p.planes[1].pixel_stride == 2 && p.planes[2].pixel_stride == 2) {
        for (uint32_t y = 0; y < h / 2; y++) {
            const uint8_t *s = c + (size_t)y * cs;
            uint8_t *d = vu + (size_t)y * w;
            if (CHROMA_U_FIRST)
                for (uint32_t x = 0; x < w; x += 2) d[x] = s[x + 1], d[x + 1] = s[x];
            else
                memcpy(d, s, w);
        }
    } else {  /* planar chroma */
        for (uint32_t y = 0; y < h / 2; y++)
            for (uint32_t x = 0; x < w / 2; x++) {
                vu[(size_t)y * w + 2 * x] = p.planes[2].data[(size_t)y * p.planes[2].row_stride + x * p.planes[2].pixel_stride];
                vu[(size_t)y * w + 2 * x + 1] = p.planes[1].data[(size_t)y * p.planes[1].row_stride + x * p.planes[1].pixel_stride];
            }
    }
    ahb_unlock(b->ahb, NULL);
    slot_lent[slot] = 1;
    return slot;
}

static void deliver_jpeg(int ok, uint64_t ts)
{
    struct cam_event e = {.type = CAM_EV_STILL, .id = still_id, .status = ok ? 0 : -5, .timestamp = ts,
                          .exposure_ns = last_exposure, .iso = last_iso};
    uint8_t *p;
    if (ok && !ahb_lock(jpeg.ahb, CPU_READ, -1, NULL, (void **)&p)) {
        struct jpeg_blob t;
        memcpy(&t, p + jpeg_capacity - sizeof(t), sizeof(t));
        if (t.id == 0xFF && t.size && t.size <= jpeg_capacity) {
            memcpy(ring + layout.jpeg_offset, p, t.size);
            e.size = t.size;
        } else {
            e.status = -5;
            LOG("jpeg without its blob header (id %#x size %u)\n", t.id, t.size);
        }
        ahb_unlock(jpeg.ahb, NULL);
    } else if (ok) {
        e.status = -5;
    }
    LOG("still %u: %d, %u bytes\n", still_id, e.status, e.size);
    send_event(&e);
}

static void result(const camera3_callback_ops_t *o, const camera3_capture_result_t *r)
{
    (void)o;
    camera_metadata_ro_entry_t e;
    if (r->result && r->frame_number == still_frame) {
        if (!find_entry(r->result, SENSOR_SENSITIVITY, &e) && e.count) last_iso = e.data.i32[0];
        if (!find_entry(r->result, SENSOR_EXPOSURE_TIME, &e) && e.count) last_exposure = e.data.i64[0];
    } else if (r->result) {
        if (!find_entry(r->result, SENSOR_SENSITIVITY, &e) && e.count) last_iso = e.data.i32[0];
    }
    if (r->result) {
        int af = last_af, ae = last_ae;
        if (!find_entry(r->result, AF_STATE, &e) && e.count) af = e.data.u8[0];
        if (!find_entry(r->result, AE_STATE, &e) && e.count) ae = e.data.u8[0];
        if (af != last_af || ae != last_ae) {
            last_af = af, last_ae = ae;
            struct cam_event ev = {.type = CAM_EV_STATE, .id = r->frame_number, .slot = (uint32_t)af,
                                   .size = (uint32_t)ae, .timestamp = shutter_ts[r->frame_number % 32]};
            send_event(&ev);
        }
    }
    for (uint32_t i = 0; i < r->num_output_buffers; i++) {
        const camera3_stream_buffer_t *sb = &r->output_buffers[i];
        wait_fence(sb->release_fence);
        struct buf *b = ours(sb);
        if (!b) continue;
        if (b == &jpeg) {
            deliver_jpeg(sb->status == 0, shutter_ts[r->frame_number % 32]);
            pthread_mutex_lock(&lock);
            jpeg.busy = 0;
            still_frame = ~0u;
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&lock);
            continue;
        }
        pthread_mutex_lock(&lock);
        int slot = sb->status == 0 && streaming ? copy_preview(b) : -1;
        b->busy = 0;
        in_flight--;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        if (slot >= 0) {
            struct cam_event ev = {.type = CAM_EV_FRAME, .slot = (uint32_t)slot, .id = r->frame_number,
                                   .timestamp = shutter_ts[r->frame_number % 32]};
            send_event(&ev);
        }
    }
}

static void notify(const camera3_callback_ops_t *o, const camera3_notify_msg_t *m)
{
    (void)o;
    if (m->type == 2) shutter_ts[m->message.shutter.frame_number % 32] = m->message.shutter.timestamp;
    if (m->type == 2 && m->message.shutter.frame_number == still_frame) {
        struct cam_event e = {.type = CAM_EV_SHUTTER, .id = still_id, .timestamp = m->message.shutter.timestamp};
        send_event(&e);
    } else if (m->type == 1) {
        LOG("error frame %u stream %p code %d\n", m->message.error.frame_number, (void *)m->message.error.stream,
            m->message.error.error_code);
        if (m->message.error.error_code == 1) {  /* ERROR_DEVICE */
            struct cam_event e = {.type = CAM_EV_ERROR, .status = -5};
            send_event(&e);
        }
    }
}

/* With halBufferMgrMode=0 the HAL doesn't ask; these hand back nothing and take buffers back. */
static int request_buffers(const camera3_callback_ops_t *o, uint32_t n, const camera3_buffer_request_t *req,
                           uint32_t *nret, camera3_stream_buffer_ret_t *ret)
{
    (void)o;
    for (uint32_t i = 0; i < n; i++) ret[i] = (camera3_stream_buffer_ret_t){req[i].stream, 1, 0, ret[i].output_buffers};
    *nret = n;
    return 2;  /* FAILED */
}

static void return_buffers(const camera3_callback_ops_t *o, uint32_t n, const camera3_stream_buffer_t *const *b)
{
    (void)o;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < n; i++) {
        struct buf *x = ours(b[i]);
        if (b[i]->release_fence >= 0) close(b[i]->release_fence);
        if (x) x->busy = 0;
    }
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static const camera3_callback_ops_t ops = {result, notify, request_buffers, return_buffers};

/* --- requests --- */

/* The request settings for the current cam_settings, from a template; with lock held. */
static void *build_settings(int template_id, int still, int torch)
{
    const void *t = dev->ops->construct_default_request_settings(dev, template_id);
    if (!t) return NULL;
    void *m = allocate_md(entry_count(t) + 16, data_count(t) + 256);
    append_md(m, t);
    const void *meta = lenses[cam][lens].meta;
    camera_metadata_ro_entry_t e;
    /* Continuous focus (the video kind while recording, which moves more gently), or a scan at the focus point that
     * then holds (AUTO, which the triggers drive). */
    uint8_t af = settings.focus != CAM_FOCUS_CONTINUOUS ? 1 : settings.video ? 3 : 4;
    if (!find_entry(meta, AF_AVAILABLE_MODES, &e))
        for (size_t i = 0; i < e.count; i++) if (e.data.u8[i] == af) { md_set(m, AF_MODE, &af, 1); break; }
    /* The point focuses and meters: a region an eighth of the image across, in the active array (with the zoom
     * ratio, the zoomed image's coordinates are the whole array's). */
    if (settings.focus_x >= 0 && settings.focus_y >= 0 && !find_entry(meta, SENSOR_INFO_ACTIVE_ARRAY, &e) && e.count >= 4) {
        int32_t w = e.data.i32[2], h = e.data.i32[3], rw = w / 8, rh = h / 8;
        int32_t x = (int32_t)(settings.focus_x * w) - rw / 2, y = (int32_t)(settings.focus_y * h) - rh / 2;
        x = x < 0 ? 0 : x > w - rw ? w - rw : x;
        y = y < 0 ? 0 : y > h - rh ? h - rh : y;
        int32_t region[5] = {x, y, x + rw - 1, y + rh - 1, 1000};
        md_set(m, AF_REGIONS, region, 5);
        md_set(m, AE_REGIONS, region, 5);
    }
    uint8_t flash = torch ? 2 : 0;
    md_set(m, FLASH_MODE, &flash, 1);
    /* Stabilisation: CHI runs EIS v3 on a video stream that asks for it, with the gyro from the sensor hub (NCS),
     * though the HAL lists only OFF among its modes. */
    uint8_t stab = settings.video && settings.stabilization;
    md_set(m, VIDEO_STABILIZATION_MODE, &stab, 1);
    int32_t ev = settings.ev;
    md_set(m, AE_EXPOSURE_COMPENSATION, &ev, 1);
    if (settings.fps_max > 0) {
        int32_t fps[2] = {settings.fps_min, settings.fps_max};
        md_set(m, AE_TARGET_FPS_RANGE, fps, 2);
    }
    /* The open lens's share of the zoom. */
    float z = zoom_now / lenses[cam][lens].ratio;
    z = z < 1 ? 1 : z > lenses[cam][lens].max_zoom ? lenses[cam][lens].max_zoom : z;
    if (!find_entry(meta, CONTROL_ZOOM_RATIO_RANGE, &e) && e.count >= 2) {
        /* A HAL with a zoom ratio takes the zoom from it, with the crop region left whole (CamX ignored the crop). */
        md_set(m, CONTROL_ZOOM_RATIO, &z, 1);
    } else if (!find_entry(meta, SENSOR_INFO_ACTIVE_ARRAY, &e) && e.count >= 4) {
        int32_t w = (int32_t)(e.data.i32[2] / z) & ~1, h = (int32_t)(e.data.i32[3] / z) & ~1;
        int32_t crop[4] = {(e.data.i32[2] - w) / 2, (e.data.i32[3] - h) / 2, w, h};
        md_set(m, SCALER_CROP_REGION, crop, 4);
    }
    if (still) {
        int32_t o = settings.jpeg_orientation;
        uint8_t q = settings.jpeg_quality > 0 && settings.jpeg_quality <= 100 ? (uint8_t)settings.jpeg_quality : 95;
        md_set(m, JPEG_ORIENTATION, &o, 1);
        md_set(m, JPEG_QUALITY, &q, 1);
    }
    return m;
}

static double since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) * 1e3 + (double)(t.tv_nsec - t0->tv_nsec) / 1e6;
}

static float clamp_zoom(float z)
{
    const struct cam_info *c = &list.camera[cam];
    return z < c->min_zoom ? c->min_zoom : z > c->max_zoom ? c->max_zoom : z;
}

/* zoom_now for this request: along the way to settings.zoom, evenly in scale. With lock held. */
static void step_zoom(void)
{
    float target = clamp_zoom(settings.zoom);
    double t = settings.zoom_ms > 0 ? since(&zoom_t0) / settings.zoom_ms : 1;
    if (t >= 1 || zoom_from <= 0) zoom_now = target;
    else zoom_now = clamp_zoom(zoom_from * powf(target / zoom_from, (float)t));
}

/* The lens for a zoom: the narrowest whose field of view still holds it. */
static int lens_for(float z)
{
    int best = -1, widest = 0;
    for (int i = 0; i < n_lenses[cam]; i++) {
        float r = lenses[cam][i].ratio;
        if (r <= z * 1.001f && (best < 0 || r > lenses[cam][best].ratio)) best = i;
        if (r < lenses[cam][widest].ratio) widest = i;
    }
    return best < 0 ? widest : best;
}

static int lens_covers(int i, float z)
{
    const struct lens *l = &lenses[cam][i];
    return l->ratio <= z * 1.001f && z <= l->ratio * l->max_zoom;
}

static int configure_lens(void);
static int open_hal(int hal);

/* Hands the camera over to another lens: the open one closed, the other opened and given the same streams. The preview
 * buffers stay; the JPEG one is the new lens's. Called from the pipeline with nothing in flight and lock not held. */
/* A lens being closed in the background after a hand-over (closing takes ~0.7 s; the new lens streams meanwhile). */
static pthread_t closer;
static int closing;

static void *close_old(void *arg)
{
    camera3_device_t *old = arg;
    old->common.close(&old->common);
    return NULL;
}

static void wait_closer(void)
{
    if (closing) pthread_join(closer, NULL);
    closing = 0;
}

static int switch_lens(int to)
{
    int from = lens;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    wait_closer();
    /* The new lens is opened and configured while the old one is still open (the HAL takes two at once), and the old
     * one closed behind it; if the HAL refuses the second, the old one is closed first. */
    camera3_device_t *old = dev;
    dev = NULL;
    int r = open_hal(lenses[cam][to].hal), both = !r;
    if (r) {
        old->common.close(&old->common);
        old = NULL;
        r = open_hal(lenses[cam][to].hal);
    }
    lens = to;
    if (!r) r = configure_lens();
    if (r) {
        LOG("lens %d (HAL %d): %d; back to %d\n", to, lenses[cam][to].hal, r, from);
        if (dev) { dev->common.close(&dev->common); dev = NULL; }
        lens = from;
        if (old) { dev = old; old = NULL; }
        else if (open_hal(lenses[cam][from].hal)) return -EIO;
        if (configure_lens()) return -EIO;
        return r;
    }
    if (old) closing = !pthread_create(&closer, NULL, close_old, old);
    if (old && !closing) old->common.close(&old->common);
    LOG("lens %d (HAL %d) in %.0f ms%s\n", to, lenses[cam][to].hal, since(&t0), both ? "" : " (the old one closed first)");
    return 0;
}

/* The pipeline's wait for results; one that lasts says what it waits for. With lock held. */
static void wait_changed(const char *why)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 3;
    if (pthread_cond_timedwait(&changed, &lock, &t) == ETIMEDOUT)
        LOG("pipeline waiting (%s): %d in flight, still %d (jpeg busy %d, frame %u), lens %d\n", why, in_flight,
            still_pending, jpeg.busy, still_frame, lens);
}

static void *pipeline(void *arg)
{
    (void)arg;
    void *torch_md = NULL;
    pthread_mutex_lock(&lock);
    while (streaming && !stopping) {
        struct buf *pb = NULL;
        for (int k = 0; k < NPREVIEW && !pb; k++) if (!preview[k].busy) pb = &preview[k];
        if (!pb || in_flight >= MAX_IN_FLIGHT || (still_pending && !still_preflash && has_jpeg && jpeg.busy)) {
            wait_changed(pb ? "in flight" : "no preview buffer");
            continue;
        }
        step_zoom();
        int want = lens_for(zoom_now);
        float end = clamp_zoom(settings.zoom);
        if (zoom_now != end) {
            /* Mid-way through a smooth zoom: one hand-over, not one per lens on the way. The open lens zooms on
             * digitally while it can; else the lens the zoom ends on takes over now, if it can show this zoom. */
            int last = lens_for(end);
            if (lens_covers(lens, zoom_now)) want = lens;
            else if (lens_covers(last, zoom_now)) want = last;
        }
        if (want != lens && !lens_failed[want]) {
            if (in_flight > 0 || jpeg.busy || still_pending) {  /* the lens's requests done first */
                wait_changed("lens hand-over");
                continue;
            }
            pthread_mutex_unlock(&lock);
            int r = switch_lens(want);
            pthread_mutex_lock(&lock);
            if (r == -EIO) {
                pthread_mutex_unlock(&lock);
                struct cam_event e = {.type = CAM_EV_ERROR, .status = r};
                send_event(&e);
                pthread_mutex_lock(&lock);
                break;
            }
            if (r) lens_failed[want] = 1;
            md_gen = ~0u;
            last_af = last_ae = -1;
            continue;
        }
        int template_id = settings.video ? TEMPLATE_RECORD : TEMPLATE_PREVIEW;
        if (md_gen != settings_gen || md_zoom != zoom_now) {
            free(preview_md); free(still_md); free(torch_md);
            preview_md = build_settings(template_id, 0, settings.flash == CAM_FLASH_TORCH);
            still_md = NULL;
            torch_md = build_settings(template_id, 0, 1);
            md_gen = settings_gen;
            md_zoom = zoom_now;
        }
        int with_still = 0;
        const void *md = preview_md;
        void *trigger_md = NULL;
        if (af_start || af_cancel) {
            uint8_t t = af_start ? 1 : 2;
            if ((trigger_md = build_settings(template_id, 0, settings.flash == CAM_FLASH_TORCH))) {
                md_set(trigger_md, AF_TRIGGER, &t, 1);
                md = trigger_md;
            }
            LOG("focus %s at %.2f,%.2f\n", af_start ? "scan" : "continuous", settings.focus_x, settings.focus_y);
            af_start = af_cancel = 0;
        }
        if (still_pending) {
            if (still_flash && still_preflash > 0) {
                still_preflash--;
                md = torch_md;
            } else if (has_jpeg && !jpeg.busy) {
                free(still_md);
                still_md = build_settings(TEMPLATE_STILL, 1, still_flash);
                md = still_md;
                with_still = 1;
            }
        }
        pb->busy = 1;
        in_flight++;
        uint32_t fn = frame_no++;
        if (with_still) {
            jpeg.busy = 1;
            still_pending = 0;
            still_frame = fn;
        }
        camera3_stream_buffer_t out[2] = {{&preview_stream, &pb->handle, 0, -1, -1, 0, 0},
                                          {&jpeg_stream, &jpeg.handle, 0, -1, -1, 0, 0}};
        camera3_capture_request_t req = {.frame_number = fn, .settings = md, .output_buffers = out,
                                         .num_output_buffers = with_still ? 2 : 1};
        pthread_mutex_unlock(&lock);
        int r = dev->ops->process_capture_request(dev, &req);
        free(trigger_md);  /* the HAL copies the settings */
        pthread_mutex_lock(&lock);
        if (r) {
            LOG("request %u: %d\n", fn, r);
            pb->busy = 0;
            in_flight--;
            if (with_still) { jpeg.busy = 0; still_frame = ~0u; }
            pthread_mutex_unlock(&lock);
            if (with_still) { struct cam_event e = {.type = CAM_EV_STILL, .id = still_id, .status = r}; send_event(&e); }
            usleep(100000);
            pthread_mutex_lock(&lock);
        }
    }
    free(torch_md);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static pthread_t pipeline_thread;

static void stop_preview(void)
{
    pthread_mutex_lock(&lock);
    int was = streaming;
    streaming = 0;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    if (!was) return;
    LOG("preview stopping\n");
    pthread_join(pipeline_thread, NULL);
    dev->ops->flush(dev);
    pthread_mutex_lock(&lock);
    for (int t = 0; t < 300 && (in_flight > 0 || jpeg.busy); t++) {
        pthread_mutex_unlock(&lock); usleep(10000); pthread_mutex_lock(&lock);
    }
    if (in_flight) LOG("%d preview buffers still with the HAL after flush\n", in_flight);
    pthread_mutex_unlock(&lock);
}

static void free_buffers(void)
{
    for (int k = 0; k < NPREVIEW; k++) if (preview[k].ahb) { ahb_release(preview[k].ahb); preview[k] = (struct buf){0}; }
    if (jpeg.ahb) { ahb_release(jpeg.ahb); jpeg = (struct buf){0}; }
    if (ring) { munmap(ring, layout.total); ring = NULL; }
    if (ring_fd >= 0) { close(ring_fd); ring_fd = -1; }
    memset(slot_lent, 0, sizeof(slot_lent));
    configured = 0;
}

/* Starts or stops the compute DSP (remoteproc "...-cdsp"). */
static void cdsp(int on)
{
    char path[128], state[16] = "";
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/name", i);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        char name[64] = "";
        int is = fgets(name, sizeof(name), f) && strstr(name, "cdsp");
        fclose(f);
        if (!is) continue;
        snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/state", i);
        if ((f = fopen(path, "re"))) { if (!fgets(state, sizeof(state), f)) state[0] = 0; fclose(f); }
        int running = !strncmp(state, "running", 7);
        if (on == running) return;
        if ((f = fopen(path, "we"))) { fputs(on ? "start" : "stop", f); fclose(f); }
        LOG("compute DSP %s\n", on ? "started" : "stopped");
        return;
    }
}

static void close_camera(void)
{
    if (!dev) return;
    stop_preview();
    wait_closer();
    LOG("close camera %d (lens %d): %d\n", cam, lens, dev->common.close(&dev->common));
    dev = NULL;
    cdsp(0);
    free_buffers();
    free(preview_md); free(still_md);
    preview_md = still_md = NULL;
    md_gen = ~0u;
    zoom_now = zoom_from = 1;
    af_start = af_cancel = 0;
    last_af = last_ae = -1;
    cam = -1;
    lens = 0;
    still_pending = 0;
    still_frame = ~0u;
}

static int open_hal(int hal)
{
    char id[8];
    snprintf(id, sizeof(id), "%d", hal);
    int r = module->common.methods->open(&module->common, id, (hw_device_t **)&dev);
    if (r) { dev = NULL; return r < 0 ? r : -EIO; }
    r = dev->ops->initialize(dev, &ops);
    if (r) { dev->common.close(&dev->common); dev = NULL; return r < 0 ? r : -EIO; }
    return 0;
}

static int open_camera(int i)
{
    if (i < 0 || (uint32_t)i >= list.count) return -EINVAL;
    close_camera();
    cdsp(1);
    int r = open_hal(lenses[i][0].hal);
    LOG("open camera %d (HAL %d): %d\n", i, lenses[i][0].hal, r);
    if (r) { cdsp(0); return r; }
    cam = i;
    lens = 0;
    memset(lens_failed, 0, sizeof(lens_failed));
    frame_no = 0;
    return 0;
}

static int alloc(struct buf *b, const camera3_stream_t *s, uint32_t w, uint32_t h)
{
    AHB_Desc d = {w, h, 1, (uint32_t)s->format, s->usage | CPU_READ | CAMERA_WRITE, 0, 0, 0};
    if (ahb_allocate(&d, &b->ahb)) return -ENOMEM;
    b->handle = ahb_handle(b->ahb);
    return 0;
}

static uint32_t jpeg_max(const void *meta)
{
    camera_metadata_ro_entry_t e;
    return find_entry(meta, JPEG_MAX_SIZE, &e) || !e.count ? 0 : (uint32_t)e.data.i32[0];
}

/* The open lens's JPEG size for the still asked for: the same if it has it, else its largest of the same shape. */
static void lens_still(uint32_t *w, uint32_t *h)
{
    *w = still_w, *h = still_h;
    if (!lens) return;
    camera_metadata_ro_entry_t e;
    uint64_t best = 0;
    if (find_entry(lenses[cam][lens].meta, STREAM_CONFIGS, &e)) return;
    for (size_t k = 0; k + 3 < e.count; k += 4) {
        const int32_t *c = e.data.i32 + k;
        if (c[0] != BLOB || c[3]) continue;
        if ((uint32_t)c[1] == still_w && (uint32_t)c[2] == still_h) { *w = still_w, *h = still_h; return; }
        int64_t d = (int64_t)c[1] * still_h - (int64_t)c[2] * still_w;
        if ((d < 0 ? -d : d) * 100 > (int64_t)c[1] * still_h || (uint64_t)c[1] * c[2] <= best) continue;
        best = (uint64_t)c[1] * c[2];
        *w = (uint32_t)c[1], *h = (uint32_t)c[2];
    }
}

/* The streams on the open lens (the preview buffers, once allocated, serve every lens); its JPEG buffer. */
static int configure_lens(void)
{
    uint32_t sw = 0, sh = 0;
    if (still_w) lens_still(&sw, &sh);
    preview_stream = (camera3_stream_t){.width = preview_stream.width, .height = preview_stream.height, .format = YUV,
                                        .usage = TEXTURE_COMPOSER | (settings.video ? VIDEO_ENCODER : 0),  /* EIS's */
                                        .physical_camera_id = "", .group_id = -1, .use_case = settings.video ? 4 : 1};
    jpeg_stream = (camera3_stream_t){.width = sw, .height = sh, .format = BLOB, .usage = CPU_READ, .data_space = JFIF,
                                     .physical_camera_id = "", .group_id = -1, .use_case = 2};
    has_jpeg = sw && sh;
    camera3_stream_t *streams[] = {&preview_stream, &jpeg_stream};
    /* The session's parameters, as Android's camera service passes them: the template's settings. Without them CHI's
     * multi-camera policy can't tell the streams' intent and the logical camera's feature graph has no sink. */
    camera3_stream_configuration_t config = {.num_streams = has_jpeg ? 2 : 1, .streams = streams,
        .session_parameters = dev->ops->construct_default_request_settings(dev,
                                                                           settings.video ? TEMPLATE_RECORD : TEMPLATE_PREVIEW)};
    int r = dev->ops->configure_streams(dev, &config);
    LOG("configure lens %d: preview %ux%u still %ux%u: %d\n", lens, preview_stream.width, preview_stream.height, sw,
        sh, r);
    if (r) return r < 0 ? r : -EINVAL;
    if (jpeg.ahb) { ahb_release(jpeg.ahb); jpeg = (struct buf){0}; }
    jpeg_capacity = 0;
    if (has_jpeg) {
        if (!(jpeg_capacity = jpeg_max(lenses[cam][lens].meta))) return -EINVAL;
        if ((r = alloc(&jpeg, &jpeg_stream, jpeg_capacity, 1))) return r;
    }
    return 0;
}

static int configure(uint32_t pw, uint32_t ph, uint32_t sw, uint32_t sh)
{
    if (!dev) return -ENODEV;
    if (!pw || !ph || (pw & 1) || (ph & 1)) return -EINVAL;
    stop_preview();
    free_buffers();
    preview_stream.width = pw, preview_stream.height = ph;
    still_w = sw, still_h = sh;
    int r = configure_lens();
    if (r) { free_buffers(); return r; }
    for (int k = 0; k < NPREVIEW; k++)
        if ((r = alloc(&preview[k], &preview_stream, pw, ph))) { free_buffers(); return r; }
    uint32_t ring_jpeg = 0;
    for (int i = 0; has_jpeg && i < n_lenses[cam]; i++)
        if (jpeg_max(lenses[cam][i].meta) > ring_jpeg) ring_jpeg = jpeg_max(lenses[cam][i].meta);
    layout = (struct cam_layout){NSLOTS, pw * ph * 3 / 2, pw, ph, 0, ring_jpeg, 0, 0};
    layout.jpeg_offset = ((uint64_t)NSLOTS * layout.slot_size + 4095) & ~4095ull;
    layout.total = (layout.jpeg_offset + ring_jpeg + 4095) & ~4095ull;
    ring_fd = (int)syscall(__NR_memfd_create, "asteroids-camera", 1u /* MFD_CLOEXEC */);
    if (ring_fd < 0 || ftruncate(ring_fd, (off_t)layout.total)) { free_buffers(); return -ENOMEM; }
    ring = mmap(NULL, layout.total, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0);
    if (ring == MAP_FAILED) { ring = NULL; free_buffers(); return -ENOMEM; }
    configured = 1;
    return 0;
}

static int start_preview(void)
{
    if (!configured) return -EINVAL;
    pthread_mutex_lock(&lock);
    if (streaming) { pthread_mutex_unlock(&lock); return 0; }
    streaming = 1;
    pthread_mutex_unlock(&lock);
    if (pthread_create(&pipeline_thread, NULL, pipeline, NULL)) { streaming = 0; return -EAGAIN; }
    LOG("preview started\n");
    return 0;
}

static int take_still(uint32_t id)
{
    if (!streaming || !has_jpeg) return -EINVAL;
    pthread_mutex_lock(&lock);
    if (still_pending || jpeg.busy) { pthread_mutex_unlock(&lock); return -EBUSY; }
    still_id = id;
    still_flash = settings.flash == CAM_FLASH_ON || settings.flash == CAM_FLASH_TORCH ||
        (settings.flash == CAM_FLASH_AUTO && last_iso >= 1600);
    still_preflash = still_flash ? PREFLASH_FRAMES : 0;
    still_pending = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    LOG("still %u asked (flash %d, iso %d)\n", id, still_flash, last_iso);
    return 0;
}

/* --- the socket --- */

static void serve(int c)
{
    struct { struct cam_req h; uint8_t data[256]; } m;
    for (;;) {
        ssize_t n = recv(c, &m, sizeof(m), 0);
        if (n <= 0) break;
        if ((size_t)n < sizeof(m.h) || m.h.magic != CAM_REQ_MAGIC || (size_t)n != sizeof(m.h) + m.h.bytes) break;
        int rc = 0;
        switch (m.h.op) {
        case CAM_LIST: reply(m.h.seq, 0, &list, sizeof(list), -1); continue;
        case CAM_OPEN: rc = open_camera(m.h.arg[0]); break;
        case CAM_CONFIGURE:
            rc = configure((uint32_t)m.h.arg[0], (uint32_t)m.h.arg[1], (uint32_t)m.h.arg[2], (uint32_t)m.h.arg[3]);
            if (!rc) { reply(m.h.seq, 0, &layout, sizeof(layout), ring_fd); continue; }
            break;
        case CAM_START: rc = start_preview(); break;
        case CAM_STOP: stop_preview(); break;
        case CAM_SETTINGS:
            if (m.h.bytes != sizeof(struct cam_settings)) { rc = -EINVAL; break; }
            pthread_mutex_lock(&lock);
            const struct cam_settings *n = (const struct cam_settings *)m.data;
            if (n->zoom != settings.zoom || n->flash != settings.flash || n->focus != settings.focus || n->video != settings.video)
                LOG("settings: zoom %.2f over %d ms, flash %d, focus %d, video %d\n", n->zoom, n->zoom_ms, n->flash,
                    n->focus, n->video);
            if (n->zoom != settings.zoom) {
                zoom_from = zoom_now;
                clock_gettime(CLOCK_MONOTONIC, &zoom_t0);
            }
            if (n->af_trigger != settings.af_trigger && n->focus_x >= 0) af_start = 1, af_cancel = 0;
            else if ((n->focus == CAM_FOCUS_CONTINUOUS && settings.focus != CAM_FOCUS_CONTINUOUS) ||
                     (n->focus_x < 0 && settings.focus_x >= 0))
                af_cancel = 1, af_start = 0;
            memcpy(&settings, m.data, sizeof(settings));
            settings_gen++;
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&lock);
            break;
        case CAM_STILL: rc = take_still((uint32_t)m.h.arg[0]); break;
        case CAM_RELEASE:
            pthread_mutex_lock(&lock);
            if ((uint32_t)m.h.arg[0] < NSLOTS) slot_lent[m.h.arg[0]] = 0;
            pthread_mutex_unlock(&lock);
            continue;
        case CAM_CLOSE: close_camera(); break;
        default: rc = -EOPNOTSUPP;
        }
        reply(m.h.seq, rc, NULL, 0, -1);
    }
}

/* Focal length over the sensor's diagonal: a lens's field of view, as a zoom. */
static float fov_scale(const void *meta)
{
    camera_metadata_ro_entry_t e;
    float f = 0, w = 0, h = 0;
    if (!find_entry(meta, LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) && e.count) memcpy(&f, e.data.u8, 4);
    if (!find_entry(meta, SENSOR_INFO_PHYSICAL_SIZE, &e) && e.count >= 2) memcpy(&w, e.data.u8, 4), memcpy(&h, e.data.u8 + 4, 4);
    return w > 0 && h > 0 ? f / sqrtf(w * w + h * h) : 0;
}

static void fill_info(int i, const void *meta, const struct camera_info *ci)
{
    struct cam_info *x = &list.camera[i];
    camera_metadata_ro_entry_t e;
    x->facing = ci->facing;
    x->orientation = ci->orientation;
    x->min_zoom = x->max_zoom = 1;
    if (!find_entry(meta, FLASH_INFO_AVAILABLE, &e) && e.count) x->flash = e.data.u8[0];
    if (!find_entry(meta, SENSOR_INFO_ACTIVE_ARRAY, &e) && e.count >= 4)
        x->active_width = (uint32_t)e.data.i32[2], x->active_height = (uint32_t)e.data.i32[3];
    if (!find_entry(meta, LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) && e.count) memcpy(&x->focal_length, e.data.u8, 4);
    if (!find_entry(meta, SCALER_MAX_DIGITAL_ZOOM, &e) && e.count) memcpy(&x->max_zoom, e.data.u8, 4);
    if (!find_entry(meta, CONTROL_ZOOM_RATIO_RANGE, &e) && e.count >= 2) {
        memcpy(&x->min_zoom, e.data.u8, 4);
        memcpy(&x->max_zoom, e.data.u8 + 4, 4);
    }
    if (!find_entry(meta, SENSOR_INFO_PHYSICAL_SIZE, &e) && e.count >= 2) {
        memcpy(&x->physical_width, e.data.u8, 4);
        memcpy(&x->physical_height, e.data.u8 + 4, 4);
    }
    if (!find_entry(meta, AE_COMPENSATION_RANGE, &e) && e.count >= 2) x->ev_min = e.data.i32[0], x->ev_max = e.data.i32[1];
    x->ev_step = 1.0f / 3;
    if (!find_entry(meta, AE_COMPENSATION_STEP, &e) && e.count && e.data.i32[1])
        x->ev_step = (float)e.data.i32[0] / (float)e.data.i32[1];
    if (!find_entry(meta, AE_AVAILABLE_TARGET_FPS_RANGES, &e))
        for (size_t k = 0; k + 1 < e.count && x->n_fps < CAM_MAX_FPS; k += 2) {
            x->fps[x->n_fps][0] = e.data.i32[k];
            x->fps[x->n_fps++][1] = e.data.i32[k + 1];
        }
    if (!find_entry(meta, STREAM_CONFIGS, &e))
        for (size_t k = 0; k + 3 < e.count; k += 4) {
            const int32_t *c = e.data.i32 + k;
            if (c[3]) continue;  /* input */
            struct cam_size *s = c[0] == YUV ? x->preview : c[0] == BLOB ? x->still : NULL;
            uint32_t *n = c[0] == YUV ? &x->n_preview : &x->n_still;
            if (!s) continue;
            int dup = 0;
            for (uint32_t k2 = 0; k2 < *n; k2++) dup |= s[k2].width == (uint32_t)c[1] && s[k2].height == (uint32_t)c[2];
            if (dup) continue;
            if (*n == CAM_MAX_SIZES) {  /* full: keep the larger ones */
                if ((uint64_t)s[*n - 1].width * s[*n - 1].height >= (uint64_t)c[1] * c[2]) continue;
                (*n)--;
            }
            uint32_t j = *n;  /* largest first */
            while (j > 0 && (uint64_t)s[j - 1].width * s[j - 1].height < (uint64_t)c[1] * c[2]) { s[j] = s[j - 1]; j--; }
            s[j] = (struct cam_size){(uint32_t)c[1], (uint32_t)c[2]};
            (*n)++;
        }
    /* The lenses' zooms against the first, by field of view (focal length over the sensor's diagonal); the camera's
     * zoom spans them all, up to 10. */
    float f0 = fov_scale(meta);
    for (int k = 0; k < n_lenses[i]; k++) {
        struct lens *l = &lenses[i][k];
        if (!find_entry(l->meta, SCALER_MAX_DIGITAL_ZOOM, &e) && e.count) memcpy(&l->max_zoom, e.data.u8, 4);
        if (!find_entry(l->meta, CONTROL_ZOOM_RATIO_RANGE, &e) && e.count >= 2) memcpy(&l->max_zoom, e.data.u8 + 4, 4);
        l->ratio = k && f0 > 0 ? fov_scale(l->meta) / f0 : 1;
        if (k) LOG("camera %d lens %d (HAL %d): zoom %.2f\n", i, k, l->hal, l->ratio);
        if (l->ratio < x->min_zoom) x->min_zoom = l->ratio;
        if (l->ratio * l->max_zoom > x->max_zoom) x->max_zoom = l->ratio * l->max_zoom;
    }
    if (n_lenses[i] > 1 && x->max_zoom > 10) x->max_zoom = 10;
    LOG("camera %d (HAL %s): facing %d orientation %d active %ux%u flash %d zoom %.2f-%.2f, %u preview and %u still sizes\n",
        i, x->hal_id, x->facing, x->orientation, x->active_width, x->active_height, x->flash, x->min_zoom, x->max_zoom,
        x->n_preview, x->n_still);
}

static void status_cb(const camera_module_callbacks_t *c, int id, int s) { (void)c; LOG("status camera %d -> %d\n", id, s); }
static void torch_cb(const camera_module_callbacks_t *c, const char *id, int s) { (void)c; (void)id; (void)s; }
static const camera_module_callbacks_t module_callbacks = {status_cb, torch_cb};

static void *server(void *arg)
{
    (void)arg;
    signal(SIGPIPE, SIG_IGN);
    void *h = dlopen("/vendor/lib64/hw/camera.qcom.so", RTLD_NOW);
    if (!h || hal3_load()) { LOG("no HAL: %s\n", dlerror()); _exit(1); }
    module = dlsym(h, "HMI");
    if (module->common.module_api_version >= 0x204 && module->init) LOG("init: %d\n", module->init());
    module->set_callbacks(&module_callbacks);
    int n = module->get_number_of_cameras();
    /* Every camera the HAL has, for choosing which to offer (CAMSERVER_CAMERAS). */
    for (int id = 0; id < n; id++) {
        struct camera_info ci = {0};
        if (module->get_camera_info(id, &ci)) continue;
        const void *meta = ci.static_camera_characteristics;
        camera_metadata_ro_entry_t e;
        char phys[64] = "", focal[64] = "";
        int logical = 0;
        float zr[2] = {0, 0};
        if (!find_entry(meta, REQUEST_AVAILABLE_CAPABILITIES, &e))
            for (size_t k = 0; k < e.count; k++) logical |= e.data.u8[k] == 11;
        if (!find_entry(meta, LOGICAL_PHYSICAL_IDS, &e))
            for (size_t k = 0, o = 0; k < e.count && o + 2 < sizeof(phys); k++) phys[o++] = e.data.u8[k] ? (char)e.data.u8[k] : ',';
        if (!find_entry(meta, LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e))
            for (size_t k = 0, o = 0; k < e.count && o + 10 < sizeof(focal); k++) {
                float f; memcpy(&f, e.data.u8 + 4 * k, 4);
                o += (size_t)snprintf(focal + o, sizeof(focal) - o, "%.2f ", f);
            }
        if (!find_entry(meta, CONTROL_ZOOM_RATIO_RANGE, &e) && e.count >= 2) memcpy(zr, e.data.u8, 8);
        uint32_t aw = 0, ah = 0;
        if (!find_entry(meta, SENSOR_INFO_ACTIVE_ARRAY, &e) && e.count >= 4) aw = (uint32_t)e.data.i32[2], ah = (uint32_t)e.data.i32[3];
        char stab[16] = "", ois[16] = "";
        if (!find_entry(meta, AVAILABLE_VIDEO_STABILIZATION_MODES, &e))
            for (size_t k = 0; k < e.count && k < 8; k++) stab[k] = (char)('0' + e.data.u8[k]);
        if (!find_entry(meta, AVAILABLE_OPTICAL_STABILIZATION, &e))
            for (size_t k = 0; k < e.count && k < 8; k++) ois[k] = (char)('0' + e.data.u8[k]);
        LOG("HAL camera %d: facing %d orientation %d %s%s focal %s zoom %.2f-%.2f active %ux%u stabilization %s ois %s\n",
            id, ci.facing, ci.orientation, logical ? "logical of " : "", phys, focal, zr[0], zr[1], aw, ah, stab, ois);
    }
    const char *want = getenv("CAMSERVER_CAMERAS") ? getenv("CAMSERVER_CAMERAS") : "0,1";
    for (const char *p = want; *p && list.count < CAM_MAX_CAMERAS;) {
        int c = (int)list.count;
        struct camera_info base = {0};
        for (;;) {  /* "0+2+3": a camera with lenses 0 (its 1x), 2 and 3 */
            int id = atoi(p);
            struct camera_info ci = {0};
            if (id >= 0 && id < n && n_lenses[c] < MAX_LENSES && !module->get_camera_info(id, &ci)) {
                if (!n_lenses[c]) base = ci;
                lenses[c][n_lenses[c]++] = (struct lens){id, ci.static_camera_characteristics, 1, 1};
            }
            while (*p && *p != ',' && *p != '+') p++;
            if (*p != '+') break;
            p++;
        }
        if (n_lenses[c]) {
            snprintf(list.camera[c].hal_id, sizeof(list.camera[0].hal_id), "%d", lenses[c][0].hal);
            fill_info(c, base.static_camera_characteristics, &base);
            list.count++;
        }
        if (*p == ',') p++;
    }
    const char *path = getenv("CAMSERVER_SOCKET") ? getenv("CAMSERVER_SOCKET") : "/hmos-socket/" CAM_SOCKET;
    uid_t allowed = getenv("CAMSERVER_UID") ? (uid_t)atoi(getenv("CAMSERVER_UID")) : 0;
    int l = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    unlink(path);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) || chown(path, allowed, allowed) || chmod(path, 0660) ||
        listen(l, 1)) {
        LOG("listen on %s: %s\n", path, strerror(errno));
        _exit(2);
    }
    LOG("ready: %u cameras on %s\n", list.count, path);
    dprintf(1, "CAMSERVER_READY=1\n");
    for (;;) {
        int c = accept4(l, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; break; }
        struct ucred cred;
        socklen_t len = sizeof(cred);
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) || (cred.uid != 0 && cred.uid != allowed)) {
            close(c);
            continue;
        }
        struct timeval tv = {2, 0};
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        LOG("client pid %d uid %d\n", cred.pid, cred.uid);
        pthread_mutex_lock(&send_lock); client = c; pthread_mutex_unlock(&send_lock);
        serve(c);
        close_camera();
        pthread_mutex_lock(&send_lock); client = -1; pthread_mutex_unlock(&send_lock);
        close(c);
        LOG("client gone\n");
    }
    _exit(3);
}

__attribute__((constructor)) static void start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, server, NULL)) _exit(1);
}
