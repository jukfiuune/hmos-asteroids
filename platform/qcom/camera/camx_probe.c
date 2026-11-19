/* camx_probe: loads Qualcomm's camera HAL (CamX, /vendor/lib64/hw/camera.qcom.so) the way Android's camera provider
 * does and lists the cameras it finds: count, facing, orientation, HAL version, number of static characteristics.
 * With CAMX_CAPTURE=<camera id> it then takes a still with that camera: a preview stream runs for 40 frames so auto
 * exposure settles, then one request adds the JPEG stream. Out: /data/capture.jpg (in the Android root), and with a
 * YUV preview stream its last frame as /data/preview.ppm.
 * Built as a preload: its constructor starts a thread that does the work and exits the process, while the host program
 * (sleep) waits. The work can't run in the constructor itself: the linker holds its lock while constructors run, and
 * CamX's worker threads dlopen and dlsym while the pipelines are built.
 *   env LD_LIBRARY_PATH=... LD_PRELOAD=/m2cam/libcamx_probe.so chroot /camroot linker64 /system/bin/sleep 600
 * The HAL3 structures are in hal3.h (CAMX_OPS=1 prints the device's ops table). Buffers come from gralloc through
 * libnativewindow's AHardwareBuffer.
 * It needs the Android services around it that the provider would have (test/camx-test.sh). */
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Straight to fd 1: the host program sets up stdout's buffering itself, and the probe's thread ends in _exit. */
#define printf(...) dprintf(1, __VA_ARGS__)

#include "hal3.h"

#define NPREVIEW 24
#define WARMUP 40

static void status(const camera_module_callbacks_t *c, int id, int s) { (void)c; printf("CAMX status camera %d -> %d\n", id, s); }
static void torch(const camera_module_callbacks_t *c, const char *id, int s) { (void)c; printf("CAMX torch %s -> %d\n", id, s); }
static const camera_module_callbacks_t callbacks = {status, torch};

/* This HAL manages its buffers itself even though it doesn't advertise it (Qualcomm's CHI stream buffer manager):
 * requests carry no buffers, the HAL asks for them through request_stream_buffers and hands them back in results or
 * through return_stream_buffers. A buffer is busy from being handed out until it comes back (and its release fence
 * has signalled). */
struct buf { AHardwareBuffer *ahb; buffer_handle_t handle; int busy, ok; };
static struct buf preview[NPREVIEW], jpeg;
static camera3_stream_t preview_stream, jpeg_stream;
static int last_preview = -1, previews_back;
static int n_asked, n_lent, n_result, n_return;                 /* buffer accounting, printed at the end */

/* Ours, by the handle: the HAL may hand back a pointer to its own copy of it. */
static struct buf *ours(const camera3_stream_buffer_t *b) {
    if (!b->buffer) return NULL;
    if (b->buffer == &jpeg.handle || *b->buffer == jpeg.handle) return &jpeg;
    for (int k = 0; k < NPREVIEW; k++)
        if (b->buffer == &preview[k].handle || *b->buffer == preview[k].handle) return &preview[k];
    return NULL;
}
static void returned(const camera3_stream_buffer_t *b) {
    if (b->release_fence >= 0) {
        struct pollfd p = {b->release_fence, POLLIN, 0};
        poll(&p, 1, 3000);
        close(b->release_fence);
    }
    struct buf *x = ours(b);
    if (!x) { printf("CAMX unknown buffer back (%p)\n", (void *)b->buffer); return; }
    __atomic_add_fetch(&n_result, 1, __ATOMIC_SEQ_CST);
    x->ok = b->status == 0;
    if (x != &jpeg && x->ok) __atomic_store_n(&last_preview, (int)(x - preview), __ATOMIC_SEQ_CST);
    if (x != &jpeg) __atomic_add_fetch(&previews_back, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&x->busy, 0, __ATOMIC_SEQ_CST);
}
static int request_buffers(const camera3_callback_ops_t *o, uint32_t n, const camera3_buffer_request_t *req,
                           uint32_t *nret, camera3_stream_buffer_ret_t *ret) {
    (void)o;
    int all = 1;
    for (uint32_t i = 0; i < n; i++) {
        int jpg = req[i].stream == &jpeg_stream;
        uint32_t got = 0;
        __atomic_add_fetch(&n_asked, (int)req[i].num_buffers_requested, __ATOMIC_SEQ_CST);
        for (int k = 0; k < (jpg ? 1 : NPREVIEW) && got < req[i].num_buffers_requested; k++) {
            struct buf *b = jpg ? &jpeg : &preview[k];
            int idle = 0;
            if (!b->ahb) continue;
            if (!__atomic_compare_exchange_n(&b->busy, &idle, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            ret[i].output_buffers[got++] = (camera3_stream_buffer_t){req[i].stream, &b->handle, 0, -1, -1, 0, 0};
        }
        __atomic_add_fetch(&n_lent, (int)got, __ATOMIC_SEQ_CST);
        ret[i].stream = req[i].stream;
        ret[i].num_output_buffers = got;
        ret[i].status = got ? 0 : 1;
        if (got < req[i].num_buffers_requested) all = 0;
    }
    *nret = n;
    return all ? 0 : 1;                                          /* OK, FAILED_PARTIAL */
}
static void return_buffers(const camera3_callback_ops_t *o, uint32_t n, const camera3_stream_buffer_t *const *b) {
    (void)o;
    for (uint32_t i = 0; i < n; i++) {
        struct buf *x = ours(b[i]);
        __atomic_add_fetch(&n_return, 1, __ATOMIC_SEQ_CST);
        if (b[i]->release_fence >= 0) close(b[i]->release_fence);
        if (x) __atomic_store_n(&x->busy, 0, __ATOMIC_SEQ_CST);
    }
}
static void result(const camera3_callback_ops_t *o, const camera3_capture_result_t *r) {
    (void)o;
    for (uint32_t i = 0; i < r->num_output_buffers; i++) returned(&r->output_buffers[i]);
}
static void notify(const camera3_callback_ops_t *o, const camera3_notify_msg_t *m) {
    (void)o;
    if (m->type == 1) printf("CAMX error frame %u stream %p code %d\n", m->message.error.frame_number,
                             (void *)m->message.error.stream, m->message.error.error_code);
}
static const camera3_callback_ops_t ops = {result, notify, request_buffers, return_buffers};

static int wait_free(struct buf *b, int ms) {
    for (; ms > 0 && __atomic_load_n(&b->busy, __ATOMIC_SEQ_CST); ms--) usleep(1000);
    return !b->busy;
}
static int wait_previews(int count, int ms) {
    for (; ms > 0 && __atomic_load_n(&previews_back, __ATOMIC_SEQ_CST) < count; ms--) usleep(1000);
    return previews_back >= count;
}

static int alloc(struct buf *b, camera3_stream_t *s, uint32_t w, uint32_t h) {
    AHB_Desc d = {w, h, 1, s->format, s->usage | CPU_READ | CAMERA_WRITE, 0, 0, 0};
    if (ahb_allocate(&d, &b->ahb)) return -1;
    b->handle = ahb_handle(b->ahb);
    return 0;
}

/* The largest size of `format` the camera outputs at most `limit` pixels. */
static void pick(const void *meta, int format, long limit, uint32_t *w, uint32_t *h) {
    camera_metadata_ro_entry_t e;
    *w = *h = 0;
    if (find_entry(meta, STREAM_CONFIGS, &e)) return;
    for (size_t i = 0; i + 3 < e.count; i += 4) {
        const int32_t *c = e.data.i32 + i;
        if (c[0] == format && !c[3] && (long)c[1] * c[2] <= limit && (long)c[1] * c[2] > (long)*w * *h)
            *w = c[1], *h = c[2];
    }
}

static void save_preview(void) {
    int i = __atomic_load_n(&last_preview, __ATOMIC_SEQ_CST);
    AHB_Planes p;
    if (preview_stream.format != YUV) return;
    if (i < 0 || ahb_lock_planes(preview[i].ahb, CPU_READ, -1, NULL, &p) || p.count < 3) {
        printf("CAMX no preview frame to save\n");
        return;
    }
    uint32_t w = preview_stream.width, h = preview_stream.height;
    printf("CAMX preview planes: Y %p row %u, Cb %p pixel %u row %u, Cr %p pixel %u row %u\n",
           (void *)p.planes[0].data, p.planes[0].row_stride, (void *)p.planes[1].data, p.planes[1].pixel_stride,
           p.planes[1].row_stride, (void *)p.planes[2].data, p.planes[2].pixel_stride, p.planes[2].row_stride);
    FILE *f = fopen("/data/preview.ppm", "we");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int Y = p.planes[0].data[y * p.planes[0].row_stride + x * p.planes[0].pixel_stride];
            int U = p.planes[1].data[y / 2 * p.planes[1].row_stride + x / 2 * p.planes[1].pixel_stride] - 128;
            int V = p.planes[2].data[y / 2 * p.planes[2].row_stride + x / 2 * p.planes[2].pixel_stride] - 128;
            int rgb[3] = {Y + 1.402 * V, Y - 0.344 * U - 0.714 * V, Y + 1.772 * U};
            for (int k = 0; k < 3; k++) fputc(rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k], f);
        }
    fclose(f);
    ahb_unlock(preview[i].ahb, NULL);
    printf("CAMX preview %ux%u saved\n", w, h);
}

static void save_jpeg(uint32_t size) {
    uint8_t *p;
    if (ahb_lock(jpeg.ahb, CPU_READ, -1, NULL, (void **)&p)) { printf("CAMX jpeg lock failed\n"); return; }
    /* camera3_jpeg_blob_t at the end of the buffer: id 0x00FF, then the JPEG's size. */
    uint16_t id; uint32_t n;
    memcpy(&id, p + size - 8, 2);
    memcpy(&n, p + size - 4, 4);
    if (id != 0xFF || n > size) printf("CAMX no JPEG blob header (id %#x size %u)\n", id, n);
    else {
        FILE *f = fopen("/data/capture.jpg", "we");
        fwrite(p, 1, n, f);
        fclose(f);
        printf("CAMX jpeg %ux%u saved: %u bytes\n", jpeg_stream.width, jpeg_stream.height, n);
    }
    ahb_unlock(jpeg.ahb, NULL);
}

static void capture(camera_module_t *m, const char *id, const void *meta) {
    camera_metadata_ro_entry_t e;
    if (find_entry(meta, JPEG_MAX_SIZE, &e) || !e.count) { printf("CAMX no jpeg max size\n"); return; }
    uint32_t jpeg_size = e.data.i32[0], pw, ph, jw, jh;
    pick(meta, YUV, 640 * 480, &pw, &ph);
    pick(meta, BLOB, 13000000, &jw, &jh);
    printf("CAMX capture camera %s: jpeg %ux%u (max %u bytes)\n", id, jw, jh, jpeg_size);
    if (!find_entry(meta, BUFFER_MANAGEMENT, &e) && e.count)
        printf("CAMX HAL buffer management advertised: version %d\n", e.data.u8[0]);

    camera3_device_t *d;
    int r = m->common.methods->open(&m->common, id, (hw_device_t **)&d);
    printf("CAMX open: %d (device version %#x)\n", r, r ? 0 : d->common.version);
    if (r) return;
    if (getenv("CAMX_OPS")) {
        Dl_info info;
        for (int i = 0; i < 12; i++) {
            void *f = ((void **)d->ops)[i];
            printf("CAMX ops[%d] %#lx\n", i, f && dladdr(f, &info) ? (long)((char *)f - (char *)info.dli_fbase) : 0);
        }
    }
    printf("CAMX initialize: %d\n", d->ops->initialize(d, &ops));
    /* The preview stream as a camera app's: implementation-defined, GPU texture and composer, 1920x1080 (a CPU-read
     * YUV stream counts as a snapshot stream to CHI and starves). Experiments: CAMX_PFMT, CAMX_PUSAGE (hex) and
     * CAMX_PSIZE (WxH; 23 3 640x480 is a YUV stream the probe can save), CAMX_NOJPEG=1 for no JPEG stream,
     * CAMX_TEMPLATE (1 preview, 2 still). */
    int pfmt = getenv("CAMX_PFMT") ? (int)strtol(getenv("CAMX_PFMT"), NULL, 16) : IMPLEMENTATION_DEFINED;
    uint32_t pusage = getenv("CAMX_PUSAGE") ? (uint32_t)strtoul(getenv("CAMX_PUSAGE"), NULL, 16) : TEXTURE_COMPOSER;
    if (pfmt == IMPLEMENTATION_DEFINED) pw = 1920, ph = 1080;
    if (getenv("CAMX_PSIZE")) sscanf(getenv("CAMX_PSIZE"), "%ux%u", &pw, &ph);
    int njpeg = getenv("CAMX_NOJPEG") ? 0 : 1;
    preview_stream = (camera3_stream_t){.width = pw, .height = ph, .format = pfmt, .usage = pusage,
                                        .physical_camera_id = "", .group_id = -1};
    jpeg_stream = (camera3_stream_t){.width = jw, .height = jh, .format = BLOB, .usage = CPU_READ, .data_space = JFIF,
                                     .physical_camera_id = "", .group_id = -1};
    camera3_stream_t *streams[] = {&preview_stream, &jpeg_stream};
    camera3_stream_configuration_t config = {.num_streams = 1 + njpeg, .streams = streams};
    r = d->ops->configure_streams(d, &config);
    printf("CAMX configure_streams: %d (preview %ux%u format %#x usage %#x max %u, jpeg usage %#x max %u)\n", r, pw, ph,
           pfmt, preview_stream.usage, preview_stream.max_buffers, jpeg_stream.usage, jpeg_stream.max_buffers);
    if (r) goto out;
    /* CAMX_NOALLOC=1: no buffers of ours (and no libui), to try the pipeline on its own. */
    for (int i = 0; i < NPREVIEW && !getenv("CAMX_NOALLOC"); i++)
        if (alloc(&preview[i], &preview_stream, pw, ph)) { printf("CAMX preview buffer allocation failed\n"); goto out; }
    if (!getenv("CAMX_NOALLOC") && alloc(&jpeg, &jpeg_stream, jpeg_size, 1)) {
        printf("CAMX jpeg buffer allocation failed\n");
        goto out;
    }
    const void *settings = d->ops->construct_default_request_settings(d,
        getenv("CAMX_TEMPLATE") ? atoi(getenv("CAMX_TEMPLATE")) : TEMPLATE_STILL);
    printf("CAMX still template: %zu entries\n", settings ? entry_count(settings) : 0);
    /* CAMX_TORCH=1: the flash on as a torch in every request, so auto exposure settles with it lit. */
    if (settings && getenv("CAMX_TORCH")) {
        void *m = allocate_md(entry_count(settings) + 4, data_count(settings) + 64);
        uint8_t torch = 2;
        append_md(m, settings);
        int r2 = md_set(m, FLASH_MODE, &torch, 1);
        printf("CAMX flash mode torch: %d\n", r2);
        settings = m;
    }

    /* At most two requests in flight; the last one adds the JPEG. */
    for (uint32_t frame = 0; frame <= WARMUP; frame++) {
        if (!wait_previews((int)frame - 2, 5000)) { printf("CAMX no preview frames back (%d)\n", previews_back); goto out; }
        /* Our buffers in the requests, as Android's provider gives them. CHI's stream buffer manager (the default
         * halBufferMgrMode) would also ask for buffers through request_stream_buffers and keep one per frame; the
         * test root turns it off (halBufferMgrMode=0), and the callback stays for when it's on. */
        struct buf *pb = NULL;
        for (int t = 0; t < 5000 && !pb; t++) {
            for (int k = 0; k < NPREVIEW && !pb; k++) {
                int idle = 0;
                if (preview[k].ahb && __atomic_compare_exchange_n(&preview[k].busy, &idle, 1, 0, __ATOMIC_SEQ_CST,
                                                                  __ATOMIC_SEQ_CST))
                    pb = &preview[k];
            }
            if (!pb) usleep(1000);
        }
        if (!pb) { printf("CAMX no free preview buffer\n"); goto out; }
        if (frame == WARMUP && njpeg) jpeg.busy = 1;
        camera3_stream_buffer_t out[2] = {{&preview_stream, &pb->handle, 0, -1, -1, 0, 0},
                                          {&jpeg_stream, &jpeg.handle, 0, -1, -1, 0, 0}};
        camera3_capture_request_t req = {.frame_number = frame, .settings = settings, .output_buffers = out,
                                         .num_output_buffers = frame == WARMUP ? 1 + njpeg : 1};
        r = d->ops->process_capture_request(d, &req);
        if (r) { printf("CAMX request %u: %d\n", frame, r); goto out; }
    }
    wait_previews(WARMUP + 1, 5000);
    printf("CAMX preview frames back: %d\n", previews_back);
    usleep(100000);
    if (!njpeg) ;
    else if (!wait_free(&jpeg, 15000)) printf("CAMX jpeg never came back\n");
    else if (!jpeg.ok) printf("CAMX jpeg came back with an error\n");
    else save_jpeg(jpeg_size);
    save_preview();
out:
    printf("CAMX buffers: callback asked %d lent %d, back in results %d, returned %d\n", n_asked, n_lent, n_result,
           n_return);
    printf("CAMX flush: %d\n", d->ops->flush(d));
    printf("CAMX close: %d\n", d->common.close(&d->common));
}

static void *probe(void *arg) {
    (void)arg;
    const char *path = getenv("CAMX_HAL") ? getenv("CAMX_HAL") : "/vendor/lib64/hw/camera.qcom.so";
    void *h = dlopen(path, RTLD_NOW);
    if (!h) { printf("CAMX dlopen failed: %s\n", dlerror()); _exit(1); }
    camera_module_t *m = dlsym(h, "HMI");
    if (hal3_load()) { printf("CAMX no libcamera_metadata or libnativewindow\n"); _exit(1); }
    printf("CAMX module %s by %s, module api %#x, hal api %#x\n", m->common.name, m->common.author,
           m->common.module_api_version, m->common.hal_api_version);
    if (m->common.module_api_version >= 0x204 && m->init) printf("CAMX init: %d\n", m->init());
    printf("CAMX set_callbacks: %d\n", m->set_callbacks(&callbacks));
    int n = m->get_number_of_cameras();
    printf("CAMX cameras: %d\n", n);
    const char *want = getenv("CAMX_CAPTURE");
    const void *want_meta = NULL;
    for (int i = 0; i < n; i++) {
        struct camera_info info = {0};
        int r = m->get_camera_info(i, &info);
        printf("CAMX camera %d: rc %d facing %d orientation %d device version %#x characteristics %zu cost %d\n", i, r,
               info.facing, info.orientation, info.device_version,
               info.static_camera_characteristics ? entry_count(info.static_camera_characteristics) : 0,
               info.resource_cost);
        if (want && atoi(want) == i) want_meta = info.static_camera_characteristics;
    }
    if (want && want_meta) capture(m, want, want_meta);
    printf("CAMX_PROBE_DONE\n");
    _exit(0);
}

__attribute__((constructor)) static void start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, probe, NULL)) { printf("CAMX no thread\n"); _exit(1); }
}
