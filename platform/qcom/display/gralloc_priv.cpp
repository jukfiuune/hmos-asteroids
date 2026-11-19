// libgralloc_priv.z.so for the anco (Android container) display path: anco_service_broker's libdelegator dlopens it
// and calls convert_buffer_handle() on each buffer the guest queues, to turn the guest's gralloc handle into a
// HarmonyOS BufferHandle for the window's surface. Kirin's version reads Kirin's gralloc handle; the guest here runs
// Nothing's QTI gralloc (common/apps/build-anco-adreno.sh), so this reads QTI's private_handle_t (commonsys-intf
// display gralloc/gr_priv_handle.h): linear buffers only (the guest's anco_spec turns UBWC off), RGB formats.
// The result has to pass our allocator's RegisterBuffer (buffer_vdi.cpp), so it carries no reserved fds or ints; the
// metadata fd stays with the guest. libdelegator frees it with FreeBufferHandle (close the fd, free()).
#include "buffer_handle.h"
#include "log.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// native_handle_t: version (its own size, 12), numFds, numInts, then the fds and the ints.
struct NativeHandle { int version, numFds, numInts, data[]; };
// private_handle_t after the native_handle_t head, up to the fields read here (the rest varies by build). The head is
// 12 bytes, so the 64-bit fields are 4 bytes off their natural alignment here: packed, as they sit in the handle.
struct __attribute__((packed, aligned(4))) QtiHandle {
    int fd, fd_metadata, magic, flags;
    int width, height;                      // allocated (aligned): width is the row pitch in pixels
    int unaligned_width, unaligned_height;  // what the client asked for
    int format, buffer_type;
    unsigned layer_count;
    uint64_t id, usage;
    unsigned size, offset;
};
static_assert(offsetof(QtiHandle, id) == 44 && offsetof(QtiHandle, offset) == 64);  // id at 56 in private_handle_t
constexpr int QTI_MAGIC = 'gmsm';
// Kirin's handle was new'd at this size and libdelegator may copy it whole: allocate as much, zeroed.
constexpr size_t OUT_SIZE = 320;

// Android HAL pixel formats to HarmonyOS PixelFormat, with bytes per pixel. Android and HarmonyOS name byte orders
// the same way (RGBA_8888: R first in memory).
bool Format(int hal, int32_t &format, int &bpp)
{
    switch (hal) {
        case 1: format = 12; bpp = 4; return true;  // RGBA_8888
        case 2: format = 11; bpp = 4; return true;  // RGBX_8888
        case 4: format = 3; bpp = 2; return true;   // RGB_565
        case 5: format = 20; bpp = 4; return true;  // BGRA_8888
        default: return false;
    }
}

// Android gralloc usage to HarmonyOS buffer usage: DMA memory the host composes and samples.
uint64_t Usage(uint64_t usage)
{
    uint64_t u = (1ULL << 3) | (1ULL << 9) | (1ULL << 10);  // MEM_DMA, HW_TEXTURE, HW_COMPOSER
    if (usage & 0xF) u |= 1ULL << 0;      // SW_READ → CPU_READ
    if (usage & 0xF0) u |= 1ULL << 1;     // SW_WRITE → CPU_WRITE
    if (usage & 0x200) u |= 1ULL << 8;    // GPU_RENDER_TARGET → HW_RENDER
    return u;
}

void Reject(const char *why, const QtiHandle *q)
{
    static int logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) >= 32) return;
    if (!q) { Ace5Log("convert_buffer_handle: %s", why); return; }
    Ace5Log("convert_buffer_handle: %s (magic=0x%x width=%d height=%d unaligned=%dx%d format=0x%x size=%u offset=%u "
        "usage=0x%llx flags=0x%x)", why, q->magic, q->width, q->height, q->unaligned_width, q->unaligned_height,
        q->format, q->size, q->offset, (unsigned long long)q->usage, q->flags);
}
}

extern "C" __attribute__((visibility("default"))) int convert_buffer_handle(void *native, void **out)
{
    if (!out) return -1;
    *out = nullptr;
    auto *n = static_cast<const NativeHandle *>(native);
    if (!n || n->version != sizeof(NativeHandle) || n->numFds < 1 || n->numFds + n->numInts < int(sizeof(QtiHandle) / 4)) {
        Reject("not a gralloc handle", nullptr);
        return -1;
    }
    auto *q = reinterpret_cast<const QtiHandle *>(n->data);
    int32_t format; int bpp;
    if (q->magic != QTI_MAGIC) { Reject("not a QTI handle", q); return -1; }
    if (!Format(q->format, format, bpp)) { Reject("unsupported format", q); return -1; }
    if (q->offset || q->layer_count > 1 || q->unaligned_width <= 0 || q->unaligned_height <= 0 ||
        q->width < q->unaligned_width || q->height < q->unaligned_height ||
        uint64_t(q->width) * bpp * q->unaligned_height > q->size || q->size > INT32_MAX) {
        Reject("unexpected layout", q);
        return -1;
    }
    struct stat st;
    if (fstat(q->fd, &st) || (st.st_size && uint64_t(st.st_size) < q->size)) { Reject("bad buffer fd", q); return -1; }

    auto *h = static_cast<BufferHandle *>(calloc(1, OUT_SIZE));
    if (!h) return -1;
    h->fd = dup(q->fd);
    if (h->fd < 0) { free(h); Reject("dup failed", q); return -1; }
    h->width = q->unaligned_width;
    h->height = q->unaligned_height;
    h->stride = q->width * bpp;
    h->size = int32_t(q->size);
    h->format = format;
    h->usage = Usage(q->usage);
    *out = h;
    static int logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 4)
        Ace5Log("convert_buffer_handle: %dx%d stride=%d size=%d format=%d usage=0x%llx (QTI format 0x%x usage 0x%llx)",
            h->width, h->height, h->stride, h->size, h->format, (unsigned long long)h->usage, q->format,
            (unsigned long long)q->usage);
    return 0;
}
