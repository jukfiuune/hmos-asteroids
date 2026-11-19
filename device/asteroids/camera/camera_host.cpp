// SPDX-License-Identifier: MIT
// asteroids: HarmonyOS's camera HDI host (ICameraHost 1.3, as camera_service in the camera device class), in front of
// camserver (platform/qcom/camera/bridge/camserver.c: Qualcomm's CamX HAL in the camera root), over the bridge's socket
// (platform/qcom/camera/bridge/protocol.h). It offers the cameras camserver lists (lcam001 the back camera, which
// zooms across its lenses, lcam002 the front one) and the rear flash as a torch while no camera is open.
//
// What camera_service expects (its source, multimedia_camera_framework; docs/camera.md):
//   - abilities: position, type, connection, sensor orientation, and the profiles as
//     OHOS_ABILITY_STREAM_AVAILABLE_EXTEND_CONFIGURATIONS: for modes 0 (normal), 1 (photo) and 2 (video), a preview
//     stream of NV21 sizes and a still stream of JPEG sizes;
//   - streams come with a buffer producer each; preview frames are copied in as NV21 (or NV12), stills as the JPEG with
//     "dataSize" and the photo keys in the buffer's extra data;
//   - after each photo, the 1.3 callback's OnCaptureReady: camera_service waits for it before the next photo (from
//     1.2 on), and Huawei's Camera app waits for the captureReady the service makes of it, also with a 1.1 operator.
//     The service hands out a 1.3 callback only to a 1.3 host and device.
// Surfaces: Huawei's libsurface is a newer build than OpenHarmony's headers, so its methods are called by their
// exported symbols (SurfaceApi) rather than through vtables that might be laid out differently.
// The stubs and the callback proxies are the donor's (libcamera_stub_1.3, libcamera_proxy_1.3); metadata goes through
// the donor's libmetadata. Stays up until SIGTERM.
#include "camera/v1_3/icamera_host.h"
#include "camera/v1_3/icamera_device.h"
#include "camera/v1_3/istream_operator.h"
#include "camera_device_ability_items.h"
#include "camera_metadata_info.h"
#include "metadata_utils.h"
#include "object_collector.h"
#include "iservmgr_hdi.h"
#include "hdf_remote_service.h"
#include "hdf_device_class.h"
#include "message_parcel.h"
#include "surface.h"
#include "surface_buffer.h"
#include "buffer_extra_data.h"
#include "native_buffer.h"

extern "C" {
#include "protocol.h"
}

#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

// Our lines go to stdout and HarmonyOS's log (hilog, tag CAMERA_HOST).
extern "C" int HiLogPrint(int type, int level, unsigned int domain, const char *tag, const char *fmt, ...)
    __attribute__((weak));  // libhilog, loaded with the HDI libraries
#define printf(...) Log(__VA_ARGS__)
static void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    if (HiLogPrint) HiLogPrint(3 /* LOG_CORE */, 4 /* LOG_INFO */, 0xD002513, "CAMERA_HOST", "%{public}s", line);
}

using namespace OHOS;
namespace Cam = OHOS::HDI::Camera::V1_0;
namespace Cam11 = OHOS::HDI::Camera::V1_1;
namespace Cam12 = OHOS::HDI::Camera::V1_2;
namespace Cam13 = OHOS::HDI::Camera::V1_3;

namespace {
constexpr char SERVICE_NAME[] = "camera_service";
constexpr char SERVICE_DESCRIPTOR[] = "ohos.hdi.camera.v1_3.ICameraHost";
constexpr char SOCKET_PATH[] = "/dev/unix/socket/" CAM_SOCKET;
const char *const CAMERA_IDS[] = {"lcam001", "lcam002", "lcam003", "lcam004"};
// The rear flash: two LEDs (torch channels 0 and 3, as the device tree's torch triggers), lit by switch 0. Brightness
// is the driver's current in mA per LED (up to 500); the torch's level (0..1) scales the full one.
constexpr const char *TORCH_LEDS[] = {"/sys/class/leds/led:torch_0/brightness", "/sys/class/leds/led:torch_3/brightness"};
constexpr char TORCH_SWITCH[] = "/sys/class/leds/led:switch_0/brightness";
constexpr int TORCH_MA = 200;
constexpr int TORCH_MA_MIN = 25;
// GraphicPixelFormat values camera_service puts in StreamInfo.format_.
constexpr int32_t PIXEL_FMT_YCBCR_420_SP = 24;  // NV12
constexpr uint64_t BUFFER_USAGE = 1 | 2 | 8;  // CPU read, CPU write, DMA memory, as the reference host asks
sem_t stopWake;

void StopSignal(int)
{
    int saved = errno;
    sem_post(&stopWake);
    errno = saved;
}

bool WriteValue(const char *path, int value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::string text = std::to_string(value);
    bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    close(fd);
    return ok;
}

// --- Huawei's libsurface, by symbol ---
struct SurfaceApi {
    sptr<Surface> (*create)(sptr<IBufferProducer> &) = nullptr;
    GSError (*request)(Surface *, sptr<SurfaceBuffer> &, int32_t &, BufferRequestConfig &) = nullptr;
    GSError (*flush)(Surface *, sptr<SurfaceBuffer> &, int32_t, BufferFlushConfig &) = nullptr;
    GSError (*cancel)(Surface *, sptr<SurfaceBuffer> &) = nullptr;
    GSError (*queueSize)(Surface *, uint32_t) = nullptr;
    void *(*virAddr)(SurfaceBuffer *) = nullptr;
    GSError (*map)(SurfaceBuffer *) = nullptr;
    int32_t (*stride)(const SurfaceBuffer *) = nullptr;
    uint32_t (*size)(const SurfaceBuffer *) = nullptr;
    GSError (*planes)(SurfaceBuffer *, void **) = nullptr;
    sptr<BufferExtraData> (*extra)(const SurfaceBuffer *) = nullptr;
    GSError (*setInt)(BufferExtraData *, const std::string &, int32_t) = nullptr;
    GSError (*setLong)(BufferExtraData *, const std::string &, int64_t) = nullptr;

    bool Load()
    {
        void *l = dlopen("libsurface.z.so", RTLD_NOW | RTLD_GLOBAL);
        if (l == nullptr)
            return false;
#define LOAD_MEMBER(member, symbol) \
        *reinterpret_cast<void **>(&(member)) = dlsym(l, symbol); \
        if ((member) == nullptr) { printf("CAMERA_HOST no %s\n", symbol); return false; }
        LOAD_MEMBER(create, "_ZN4OHOS7Surface23CreateSurfaceAsProducerERNS_4sptrINS_15IBufferProducerEEE");
        LOAD_MEMBER(request, "_ZN4OHOS15ProducerSurface13RequestBufferERNS_4sptrINS_13SurfaceBufferEEERiRNS_19BufferRequestConfigE");
        LOAD_MEMBER(flush, "_ZN4OHOS15ProducerSurface11FlushBufferERNS_4sptrINS_13SurfaceBufferEEEiRNS_17BufferFlushConfigE");
        LOAD_MEMBER(cancel, "_ZN4OHOS15ProducerSurface12CancelBufferERNS_4sptrINS_13SurfaceBufferEEE");
        LOAD_MEMBER(queueSize, "_ZN4OHOS15ProducerSurface12SetQueueSizeEj");
        LOAD_MEMBER(virAddr, "_ZN4OHOS17SurfaceBufferImpl10GetVirAddrEv");
        LOAD_MEMBER(map, "_ZN4OHOS17SurfaceBufferImpl3MapEv");
        LOAD_MEMBER(stride, "_ZNK4OHOS17SurfaceBufferImpl9GetStrideEv");
        LOAD_MEMBER(size, "_ZNK4OHOS17SurfaceBufferImpl7GetSizeEv");
        LOAD_MEMBER(planes, "_ZN4OHOS17SurfaceBufferImpl13GetPlanesInfoEPPv");
        LOAD_MEMBER(extra, "_ZNK4OHOS17SurfaceBufferImpl12GetExtraDataEv");
        LOAD_MEMBER(setInt, "_ZN4OHOS19BufferExtraDataImpl8ExtraSetERKNSt3__h12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEi");
        LOAD_MEMBER(setLong, "_ZN4OHOS19BufferExtraDataImpl8ExtraSetERKNSt3__h12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEl");
#undef LOAD_MEMBER
        return true;
    }
} surfaces;

// The request and flush configs, with room past the fields we know for a newer library's.
struct RequestConfig { BufferRequestConfig c; uint8_t spare[64]; };
struct FlushConfig { BufferFlushConfig c; uint8_t spare[64]; };

int64_t NowBoottime()
{
    timespec t{};
    clock_gettime(CLOCK_BOOTTIME, &t);
    return static_cast<int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

// --- the bridge to camserver ---
class Bridge {
public:
    using EventHandler = std::function<void(const cam_event &)>;

    ~Bridge() { Disconnect(); }

    int Connect()
    {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof(a.sun_path), "%s", SOCKET_PATH);
        if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
            int e = -errno;
            if (fd >= 0) close(fd);
            return e;
        }
        fd_ = fd;
        reader_ = std::thread([this] { Read(); });
        return 0;
    }

    void Disconnect()
    {
        if (fd_ < 0) return;
        shutdown(fd_, SHUT_RDWR);
        if (reader_.joinable()) reader_.join();
        close(fd_);
        fd_ = -1;
    }

    void SetHandler(EventHandler h)
    {
        std::lock_guard<std::mutex> l(handlerMutex_);
        handler_ = std::move(h);
    }

    // A request and its reply (data copied to out); *fd gets a passed descriptor, if any.
    int Call(uint32_t op, std::initializer_list<int32_t> args = {}, const void *data = nullptr, uint32_t bytes = 0,
             void *out = nullptr, size_t outSize = 0, int *fd = nullptr)
    {
        std::lock_guard<std::mutex> call(callMutex_);
        if (fd_ < 0) return -ENOTCONN;
        struct { cam_req h; uint8_t d[256]; } m{};
        m.h = {CAM_REQ_MAGIC, op, ++seq_, bytes, {0, 0, 0, 0}};
        int i = 0;
        for (int32_t v : args) m.h.arg[i++] = v;
        if (bytes) std::memcpy(m.d, data, bytes);
        std::unique_lock<std::mutex> l(replyMutex_);
        replied_ = false;
        if (send(fd_, &m, sizeof(m.h) + bytes, MSG_NOSIGNAL) < 0) return -errno;
        if (op == CAM_RELEASE) return 0;
        if (!replyCond_.wait_for(l, std::chrono::seconds(10), [this] { return replied_ || dead_; })) return -ETIMEDOUT;
        if (!replied_) return -EPIPE;
        if (out) std::memcpy(out, reply_.data(), std::min(outSize, reply_.size()));
        if (fd) *fd = replyFd_; else if (replyFd_ >= 0) close(replyFd_);
        replyFd_ = -1;
        return replyRc_;
    }

    bool Alive() const { return fd_ >= 0 && !dead_; }

private:
    void Read()
    {
        std::vector<uint8_t> buf(sizeof(cam_reply) + sizeof(cam_list));
        for (;;) {
            char control[CMSG_SPACE(sizeof(int))];
            iovec iov{buf.data(), buf.size()};
            msghdr mh{};
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = control;
            mh.msg_controllen = sizeof(control);
            ssize_t n = recvmsg(fd_, &mh, 0);
            if (n <= 0) break;
            uint32_t magic;
            std::memcpy(&magic, buf.data(), 4);
            if (magic == CAM_EVENT_MAGIC && n == sizeof(cam_event)) {
                cam_event e;
                std::memcpy(&e, buf.data(), sizeof(e));
                EventHandler h;
                {
                    std::lock_guard<std::mutex> l(handlerMutex_);
                    h = handler_;
                }
                if (h) h(e);
                else if (e.type == CAM_EV_FRAME) Release(e.slot);
                continue;
            }
            if (magic != CAM_REPLY_MAGIC || n < static_cast<ssize_t>(sizeof(cam_reply))) continue;
            cam_reply r;
            std::memcpy(&r, buf.data(), sizeof(r));
            std::lock_guard<std::mutex> l(replyMutex_);
            replyRc_ = r.rc;
            reply_.assign(buf.begin() + sizeof(r), buf.begin() + n);
            cmsghdr *c = CMSG_FIRSTHDR(&mh);
            replyFd_ = -1;
            if (c && c->cmsg_type == SCM_RIGHTS) std::memcpy(&replyFd_, CMSG_DATA(c), sizeof(int));
            replied_ = true;
            replyCond_.notify_all();
        }
        std::lock_guard<std::mutex> l(replyMutex_);
        dead_ = true;
        replyCond_.notify_all();
        EventHandler h;
        {
            std::lock_guard<std::mutex> hl(handlerMutex_);
            h = handler_;
        }
        if (h) {
            cam_event e{};
            e.type = CAM_EV_ERROR;
            e.status = -EPIPE;
            h(e);
        }
    }

public:
    void Release(uint32_t slot)
    {
        cam_req h{CAM_REQ_MAGIC, CAM_RELEASE, 0, 0, {static_cast<int32_t>(slot), 0, 0, 0}};
        if (fd_ >= 0) send(fd_, &h, sizeof(h), MSG_NOSIGNAL | MSG_DONTWAIT);
    }

private:
    int fd_ = -1;
    uint32_t seq_ = 0;
    std::thread reader_;
    std::mutex callMutex_, replyMutex_, handlerMutex_;
    std::condition_variable replyCond_;
    bool replied_ = false;
    std::atomic<bool> dead_{false};
    int32_t replyRc_ = 0;
    std::vector<uint8_t> reply_;
    int replyFd_ = -1;
    EventHandler handler_;
};

cam_list g_list{};

// --- settings from camera_service's metadata ---
// Touch /data/local/tmp/camera-host-tags to log the tags of each settings update. A capture's settings repeat what
// the session has set, so from a capture (`repeated`) the same focus point doesn't start another scan.
void ApplyMetadata(const std::vector<uint8_t> &blob, cam_settings &s, bool repeated = false)
{
    if (blob.empty()) return;
    std::shared_ptr<Camera::CameraMetadata> m;
    Camera::MetadataUtils::ConvertVecToMetadata(blob, m);
    if (m == nullptr) return;
    camera_metadata_item_t item;
    common_metadata_header_t *h = m->get();
    if (getenv("CAMERA_HOST_TAGS") || access("/data/local/tmp/camera-host-tags", F_OK) == 0) {
        std::string tags;
        for (uint32_t i = 0; i < Camera::GetCameraMetadataItemCount(h) && i < 40; i++)
            if (Camera::GetCameraMetadataItem(h, i, &item) == 0) tags += " " + std::to_string(item.item);
        printf("CAMERA_HOST settings:%s\n", tags.c_str());
    }
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_FLASH_MODE, &item) == 0 && item.count) {
        switch (item.data.u8[0]) {
        case OHOS_CAMERA_FLASH_MODE_OPEN: s.flash = CAM_FLASH_ON; break;
        case OHOS_CAMERA_FLASH_MODE_AUTO: s.flash = CAM_FLASH_AUTO; break;
        case OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN: s.flash = CAM_FLASH_TORCH; break;
        default: s.flash = CAM_FLASH_OFF;
        }
    }
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_ZOOM_RATIO, &item) == 0 && item.count) {
        s.zoom = item.data.f[0];
        s.zoom_ms = 0;
    }
    // The app's zoom buttons zoom smoothly: camera_service sends (ratio x 100, ms from now) steps for the HAL to go
    // through; camserver goes from where it is to the last over the same time.
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_SMOOTH_ZOOM_RATIOS, &item) == 0 && item.count > 1) {
        s.zoom = item.data.ui32[item.count - 2] / 100.0f;
        s.zoom_ms = static_cast<int32_t>(item.data.ui32[item.count - 1]);
    }
    // Focus: the app sets AUTO and then a point (normalized, across the sensor's image) to focus there, and goes back
    // to CONTINUOUS_AUTO later; the point also meters.
    bool point = Camera::FindCameraMetadataItem(h, OHOS_CONTROL_AF_REGIONS, &item) == 0 && item.count >= 2;
    float x = point ? item.data.f[0] : -1, y = point ? item.data.f[1] : -1;
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_FOCUS_MODE, &item) == 0 && item.count) {
        int32_t focus = item.data.u8[0] == OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO ? CAM_FOCUS_CONTINUOUS
            : item.data.u8[0] == OHOS_CAMERA_FOCUS_MODE_AUTO ? CAM_FOCUS_AUTO : CAM_FOCUS_LOCKED;
        if (focus == CAM_FOCUS_CONTINUOUS && s.focus != CAM_FOCUS_CONTINUOUS && !point) s.focus_x = s.focus_y = -1;
        s.focus = focus;
    }
    if (!point && Camera::FindCameraMetadataItem(h, OHOS_CONTROL_AE_REGIONS, &item) == 0 && item.count >= 2) {
        s.focus_x = item.data.f[0];  // a metering point alone: no scan
        s.focus_y = item.data.f[1];
    }
    if (point && (!repeated || x != s.focus_x || y != s.focus_y)) {
        s.focus_x = x;
        s.focus_y = y;
        s.af_trigger++;
    }
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_AE_EXPOSURE_COMPENSATION, &item) == 0 && item.count)
        s.ev = item.data.i32[0];
    if (Camera::FindCameraMetadataItem(h, OHOS_JPEG_ORIENTATION, &item) == 0 && item.count)
        s.jpeg_orientation = item.data.i32[0];
    if (Camera::FindCameraMetadataItem(h, OHOS_JPEG_QUALITY, &item) == 0 && item.count)
        s.jpeg_quality = item.data.u8[0] == 0 ? 70 : item.data.u8[0] == 1 ? 85 : 95;  // LOW, MIDDLE, HIGH
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_VIDEO_STABILIZATION_MODE, &item) == 0 && item.count)
        s.stabilization = item.data.u8[0] != OHOS_CAMERA_VIDEO_STABILIZATION_OFF;
    if (Camera::FindCameraMetadataItem(h, OHOS_CONTROL_FPS_RANGES, &item) == 0 && item.count >= 2) {
        s.fps_min = item.data.i32[0];
        s.fps_max = item.data.i32[1];
    }
}

// --- the stream operator ---
struct Stream {
    int32_t id, width, height, format, intent;
    sptr<Surface> surface;
    uint32_t frames = 0;
    int lastSkip = 0;
};

class StreamOperator final : public Cam13::IStreamOperator {
public:
    StreamOperator(std::shared_ptr<Bridge> bridge, const sptr<Cam::IStreamOperatorCallback> &cb,
                   const sptr<Cam13::IStreamOperatorCallback> &cb13, cam_settings *settings, std::mutex *settingsMutex)
        : bridge_(std::move(bridge)), cb_(cb), cb13_(cb13), settings_(settings), settingsMutex_(settingsMutex)
    {
        bridge_->SetHandler([this](const cam_event &e) { OnEvent(e); });
    }
    ~StreamOperator() override
    {
        bridge_->SetHandler(nullptr);
        Unmap();
    }

    // A new session on the open device (the app switching between Photo and Video) asks for the stream operator
    // again with its own callback; the old session's no longer knows the streams (camera_service: "OnCaptureStarted
    // StreamId: 2 not found", and Huawei's app keeps its shutter locked waiting for the preview's first frame).
    // Called between sessions, with no capture running.
    void SetCallbacks(const sptr<Cam::IStreamOperatorCallback> &cb, const sptr<Cam13::IStreamOperatorCallback> &cb13)
    {
        std::lock_guard<std::mutex> l(mutex_);
        cb_ = cb;
        cb13_ = cb13;
    }

    // 1.3 with a 1.3 callback, to which each photo's OnCaptureReady goes (CaptureReady); else 1.1: from 1.2 on,
    // camera_service waits for that callback before the next photo.
    int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) override
    {
        majorVer = 1;
        minorVer = cb13_ != nullptr ? 3 : 1;
        return 0;
    }

    int32_t EnableResult(int32_t, const std::vector<uint8_t> &) override { return Cam::NO_ERROR; }
    int32_t DisableResult(int32_t, const std::vector<uint8_t> &) override { return Cam::NO_ERROR; }

    int32_t IsStreamsSupported(Cam::OperationMode, const std::vector<uint8_t> &, const std::vector<Cam::StreamInfo> &,
                               Cam::StreamSupportType &type) override
    {
        type = Cam::DYNAMIC_SUPPORTED;
        return Cam::NO_ERROR;
    }
    int32_t IsStreamsSupported_V1_1(Cam11::OperationMode_V1_1, const std::vector<uint8_t> &,
                                    const std::vector<Cam11::StreamInfo_V1_1> &, Cam::StreamSupportType &type) override
    {
        type = Cam::DYNAMIC_SUPPORTED;
        return Cam::NO_ERROR;
    }

    int32_t CreateStreams(const std::vector<Cam::StreamInfo> &infos) override
    {
        std::lock_guard<std::mutex> l(mutex_);
        for (const auto &i : infos) {
            Stream s{i.streamId_, i.width_, i.height_, i.format_, i.intent_, nullptr};
            if (i.bufferQueue_ != nullptr && i.bufferQueue_->producer_ != nullptr) {
                sptr<IBufferProducer> p = i.bufferQueue_->producer_;
                s.surface = surfaces.create(p);
            }
            printf("CAMERA_HOST stream %d: %dx%d format %d intent %d surface %d\n", s.id, s.width, s.height,
                        s.format, s.intent, s.surface != nullptr);
            streams_[s.id] = s;
        }
        return Cam::NO_ERROR;
    }
    int32_t CreateStreams_V1_1(const std::vector<Cam11::StreamInfo_V1_1> &infos) override
    {
        std::vector<Cam::StreamInfo> v;
        for (const auto &i : infos) v.push_back(i.v1_0);
        return CreateStreams(v);
    }
    int32_t UpdateStreams(const std::vector<Cam11::StreamInfo_V1_1> &infos) override { return CreateStreams_V1_1(infos); }

    int32_t ReleaseStreams(const std::vector<int32_t> &ids) override
    {
        bool empty;
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (int32_t id : ids) streams_.erase(id);
            empty = streams_.empty();
        }
        if (empty) {
            bridge_->Call(CAM_STOP);
            std::lock_guard<std::mutex> l(mutex_);
            running_ = false;
            repeating_.clear();
        }
        return Cam::NO_ERROR;
    }

    int32_t CommitStreams(Cam::OperationMode, const std::vector<uint8_t> &) override { return Commit(); }
    int32_t CommitStreams_V1_1(Cam11::OperationMode_V1_1, const std::vector<uint8_t> &) override { return Commit(); }

    int32_t GetStreamAttributes(std::vector<Cam::StreamAttribute> &attributes) override
    {
        std::lock_guard<std::mutex> l(mutex_);
        attributes.clear();
        for (auto &[id, s] : streams_) {
            Cam::StreamAttribute a{};
            a.streamId_ = id;
            a.width_ = s.width;
            a.height_ = s.height;
            a.overrideFormat_ = s.format;
            a.overrideDataspace_ = 0;
            a.producerUsage_ = BUFFER_USAGE;
            a.producerBufferCount_ = 8;
            a.maxBatchCaptureCount_ = 1;
            a.maxCaptureCount_ = 1;
            attributes.push_back(a);
        }
        return Cam::NO_ERROR;
    }
    int32_t AttachBufferQueue(int32_t streamId, const sptr<Cam::BufferProducerSequenceable> &producer) override
    {
        std::lock_guard<std::mutex> l(mutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end() || producer == nullptr || producer->producer_ == nullptr) return Cam::INVALID_ARGUMENT;
        sptr<IBufferProducer> p = producer->producer_;
        it->second.surface = surfaces.create(p);
        printf("CAMERA_HOST stream %d: surface attached\n", streamId);
        return Cam::NO_ERROR;
    }
    int32_t DetachBufferQueue(int32_t streamId) override
    {
        std::lock_guard<std::mutex> l(mutex_);
        auto it = streams_.find(streamId);
        if (it != streams_.end()) it->second.surface = nullptr;
        return Cam::NO_ERROR;
    }

    int32_t Capture(int32_t captureId, const Cam::CaptureInfo &info, bool isStreaming) override
    {
        {
            std::lock_guard<std::mutex> l(*settingsMutex_);
            ApplyMetadata(info.captureSetting_, *settings_, true);
            bridge_->Call(CAM_SETTINGS, {}, settings_, sizeof(*settings_));
        }
        int32_t still = -1;
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (int32_t id : info.streamIds_) {
                auto it = streams_.find(id);
                if (it == streams_.end()) return Cam::INVALID_ARGUMENT;
                if (it->second.intent == Cam::STILL_CAPTURE) still = id;
            }
            if (!configured_) return Cam::CAMERA_CLOSED;
        }
        int rc = EnsureRunning();
        if (rc) return Cam::DEVICE_ERROR;
        if (isStreaming) {
            std::lock_guard<std::mutex> l(mutex_);
            repeating_[captureId] = info.streamIds_;
        } else if (still >= 0) {
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (stillCapture_ >= 0) return Cam::CAMERA_BUSY;
                stillCapture_ = captureId;
                stillStream_ = still;
            }
            rc = bridge_->Call(CAM_STILL, {captureId});
            if (rc) {
                std::lock_guard<std::mutex> l(mutex_);
                stillCapture_ = -1;
                printf("CAMERA_HOST still %d: %d\n", captureId, rc);
                return Cam::DEVICE_ERROR;
            }
        }
        printf("CAMERA_HOST capture %d streaming %d streams %zu still %d\n", captureId, isStreaming,
                    info.streamIds_.size(), still);
        if (cb_ != nullptr) cb_->OnCaptureStarted(captureId, info.streamIds_);
        return Cam::NO_ERROR;
    }

    int32_t CancelCapture(int32_t captureId) override
    {
        std::vector<Cam::CaptureEndedInfo> ended;
        bool idle;
        {
            std::lock_guard<std::mutex> l(mutex_);
            auto it = repeating_.find(captureId);
            if (it == repeating_.end()) return Cam::NO_ERROR;
            for (int32_t id : it->second) {
                auto s = streams_.find(id);
                ended.push_back({id, s != streams_.end() ? static_cast<int32_t>(s->second.frames) : 0});
            }
            repeating_.erase(it);
            idle = repeating_.empty() && stillCapture_ < 0;
        }
        if (idle) StopRunning();
        printf("CAMERA_HOST cancel %d\n", captureId);
        if (cb_ != nullptr) cb_->OnCaptureEnded(captureId, ended);
        return Cam::NO_ERROR;
    }

    int32_t ChangeToOfflineStream(const std::vector<int32_t> &, const sptr<Cam::IStreamOperatorCallback> &,
                                  sptr<Cam::IOfflineStreamOperator> &offline) override
    {
        offline = nullptr;
        return Cam::METHOD_NOT_SUPPORTED;
    }
    int32_t ConfirmCapture(int32_t) override { return Cam::NO_ERROR; }

    void Close()
    {
        {
            std::lock_guard<std::mutex> l(mutex_);
            repeating_.clear();
            streams_.clear();
            stillCapture_ = -1;
        }
        StopRunning();
    }

private:
    int32_t Commit()
    {
        int32_t pw = 0, ph = 0, sw = 0, sh = 0, video = 0;
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (auto &[id, s] : streams_) {
                if ((s.intent == Cam::PREVIEW || s.intent == Cam::VIDEO) &&
                    static_cast<int64_t>(s.width) * s.height > static_cast<int64_t>(pw) * ph)
                    pw = s.width, ph = s.height;
                if (s.intent == Cam::STILL_CAPTURE) sw = s.width, sh = s.height;
                video |= s.intent == Cam::VIDEO;
            }
        }
        {
            // A video session: the recording template and video focus.
            std::lock_guard<std::mutex> l(*settingsMutex_);
            settings_->video = video;
            bridge_->Call(CAM_SETTINGS, {}, settings_, sizeof(*settings_));
        }
        if (!pw) pw = 1920, ph = 1080;  // a photo-only session: the still is taken from a running preview
        StopRunning();
        Unmap();
        cam_layout layout{};
        int fd = -1;
        int rc = bridge_->Call(CAM_CONFIGURE, {pw, ph, sw, sh}, nullptr, 0, &layout, sizeof(layout), &fd);
        printf("CAMERA_HOST commit preview %dx%d still %dx%d: %d\n", pw, ph, sw, sh, rc);
        if (rc || fd < 0) {
            if (fd >= 0) close(fd);
            return Cam::INVALID_ARGUMENT;
        }
        void *p = mmap(nullptr, layout.total, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return Cam::INSUFFICIENT_RESOURCES;
        std::lock_guard<std::mutex> l(mutex_);
        ring_ = static_cast<const uint8_t *>(p);
        layout_ = layout;
        configured_ = true;
        return Cam::NO_ERROR;
    }

    void Unmap()
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (ring_) munmap(const_cast<uint8_t *>(ring_), layout_.total);
        ring_ = nullptr;
        configured_ = false;
    }

    int EnsureRunning()
    {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (running_) return 0;
        }
        int rc = bridge_->Call(CAM_START);
        std::lock_guard<std::mutex> l(mutex_);
        running_ = rc == 0;
        return rc;
    }

    void StopRunning()
    {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (!running_) return;
            running_ = false;
        }
        bridge_->Call(CAM_STOP);
    }

    // A preview frame (NV21 in the slot) into a stream's next buffer.
    void WriteFrame(Stream &s, const uint8_t *nv21, uint64_t ts)
    {
        auto skip = [&s](int why) {
            if (s.lastSkip != why) printf("CAMERA_HOST stream %d: frame not written (%d)\n", s.id, why);
            s.lastSkip = why;
        };
        if (s.surface == nullptr) return skip(1);
        if (s.width != static_cast<int32_t>(layout_.width) || s.height != static_cast<int32_t>(layout_.height))
            return skip(2);
        RequestConfig rc{};
        rc.c.width = s.width;
        rc.c.height = s.height;
        rc.c.strideAlignment = 8;
        rc.c.format = s.format;
        rc.c.usage = BUFFER_USAGE;
        rc.c.timeout = 0;
        rc.c.colorGamut = GRAPHIC_COLOR_GAMUT_SRGB;
        rc.c.transform = GRAPHIC_ROTATE_NONE;
        sptr<SurfaceBuffer> b;
        int32_t fence = -1;
        GSError re = surfaces.request(s.surface.GetRefPtr(), b, fence, rc.c);
        if (re != GSERROR_OK || b == nullptr) return skip(1000 + re);
        if (fence >= 0) close(fence);
        uint8_t *dst = static_cast<uint8_t *>(surfaces.virAddr(b.GetRefPtr()));
        if (dst == nullptr && surfaces.map(b.GetRefPtr()) == GSERROR_OK)
            dst = static_cast<uint8_t *>(surfaces.virAddr(b.GetRefPtr()));
        if (dst == nullptr) {
            surfaces.cancel(s.surface.GetRefPtr(), b);
            return skip(3);
        }
        uint32_t w = layout_.width, h = layout_.height;
        size_t yStride = static_cast<size_t>(surfaces.stride(b.GetRefPtr()));
        size_t uvOffset = yStride * h, uvStride = yStride;
        void *pi = nullptr;
        if (surfaces.planes(b.GetRefPtr(), &pi) == GSERROR_OK && pi != nullptr) {
            auto *planes = static_cast<OH_NativeBuffer_Planes *>(pi);
            // Only the chroma plane's offset: Huawei's plane records have the row and pixel strides the other way
            // round from OpenHarmony's header (rows of 1 and 2 bytes); the row stride is the buffer's.
            if (planes->planeCount >= 2)
                uvOffset = planes->planeCount < 3 || planes->planes[1].offset < planes->planes[2].offset
                    ? planes->planes[1].offset : planes->planes[2].offset;
        }
        if (yStride < w || uvOffset + uvStride * (h / 2) > surfaces.size(b.GetRefPtr())) {
            printf("CAMERA_HOST stream %d: buffer layout stride %zu uv %zu/%zu size %u\n", s.id, yStride, uvOffset,
                   uvStride, surfaces.size(b.GetRefPtr()));
            surfaces.cancel(s.surface.GetRefPtr(), b);
            return skip(4);
        }
        for (uint32_t y = 0; y < h; y++) std::memcpy(dst + y * yStride, nv21 + static_cast<size_t>(y) * w, w);
        const uint8_t *vu = nv21 + static_cast<size_t>(w) * h;
        for (uint32_t y = 0; y < h / 2; y++) {
            uint8_t *d = dst + uvOffset + y * uvStride;
            const uint8_t *src = vu + static_cast<size_t>(y) * w;
            if (s.format == PIXEL_FMT_YCBCR_420_SP)
                for (uint32_t x = 0; x < w; x += 2) d[x] = src[x + 1], d[x + 1] = src[x];
            else
                std::memcpy(d, src, w);
        }
        sptr<BufferExtraData> extra = surfaces.extra(b.GetRefPtr());
        if (extra != nullptr) {
            surfaces.setLong(extra.GetRefPtr(), "timeStamp", static_cast<int64_t>(ts));
            surfaces.setInt(extra.GetRefPtr(), "streamId", s.id);
        }
        FlushConfig fc{};
        fc.c.damage = {0, 0, s.width, s.height};
        fc.c.timestamp = static_cast<int64_t>(ts);
        GSError fe = surfaces.flush(s.surface.GetRefPtr(), b, -1, fc.c);
        if (fe != GSERROR_OK) return skip(2000 + fe);
        if (s.frames++ == 0) printf("CAMERA_HOST stream %d: first frame (stride %zu, uv at %zu)\n", s.id, yStride, uvOffset);
        s.lastSkip = 0;
    }

    // The JPEG into the still stream's buffer. Returns 0 or a CamRetCode.
    int WriteJpeg(Stream &s, int32_t captureId, const uint8_t *jpeg, uint32_t size, uint64_t ts)
    {
        if (s.surface == nullptr) return Cam::INVALID_ARGUMENT;
        // Our allocator (platform/qcom/display/buffer_vdi.cpp) takes BLOB as bytes in one row; width x height bytes
        // holds any JPEG of that size. The photo's dimensions go in the extra data.
        RequestConfig rc{};
        rc.c.width = s.width * s.height;
        rc.c.height = 1;
        rc.c.strideAlignment = 8;
        rc.c.format = s.format;
        rc.c.usage = BUFFER_USAGE;
        rc.c.timeout = 0;
        rc.c.colorGamut = GRAPHIC_COLOR_GAMUT_SRGB;
        rc.c.transform = GRAPHIC_ROTATE_NONE;
        sptr<SurfaceBuffer> b;
        int32_t fence = -1;
        GSError e = GSERROR_NO_BUFFER;
        for (int t = 0; t < 100 && (e = surfaces.request(s.surface.GetRefPtr(), b, fence, rc.c)) != GSERROR_OK; t++)
            usleep(10000);
        if (e != GSERROR_OK || b == nullptr) {
            printf("CAMERA_HOST jpeg: no buffer (%d)\n", e);
            return Cam::INSUFFICIENT_RESOURCES;
        }
        if (fence >= 0) close(fence);
        uint8_t *dst = static_cast<uint8_t *>(surfaces.virAddr(b.GetRefPtr()));
        if (dst == nullptr && surfaces.map(b.GetRefPtr()) == GSERROR_OK)
            dst = static_cast<uint8_t *>(surfaces.virAddr(b.GetRefPtr()));
        uint32_t capacity = surfaces.size(b.GetRefPtr());
        if (dst == nullptr || size > capacity) {
            printf("CAMERA_HOST jpeg: %u bytes into a %u byte buffer\n", size, capacity);
            surfaces.cancel(s.surface.GetRefPtr(), b);
            return Cam::INSUFFICIENT_RESOURCES;
        }
        std::memcpy(dst, jpeg, size);
        static std::atomic<int64_t> imageIds{0};
        sptr<BufferExtraData> extra = surfaces.extra(b.GetRefPtr());
        if (extra != nullptr) {
            BufferExtraData *x = extra.GetRefPtr();
            surfaces.setInt(x, "dataSize", static_cast<int32_t>(size));
            surfaces.setLong(x, "timeStamp", static_cast<int64_t>(ts ? ts : NowBoottime()));
            surfaces.setInt(x, "isKeyFrame", 0);
            surfaces.setInt(x, "streamId", s.id);
            surfaces.setInt(x, "captureId", captureId);
            surfaces.setInt(x, "dataWidth", s.width);
            surfaces.setInt(x, "dataHeight", s.height);
            surfaces.setLong(x, "imageId", NowBoottime() + (++imageIds));
            surfaces.setInt(x, "isDegradedImage", 0);
            surfaces.setInt(x, "deferredProcessingType", 0);
            surfaces.setInt(x, "deferredImageFormat", 1);
        }
        FlushConfig fc{};
        fc.c.damage = {0, 0, s.width, s.height};
        fc.c.timestamp = static_cast<int64_t>(ts);
        e = surfaces.flush(s.surface.GetRefPtr(), b, -1, fc.c);
        printf("CAMERA_HOST jpeg %u bytes into stream %d (buffer %u): %d\n", size, s.id, capacity, e);
        s.frames++;
        return e == GSERROR_OK ? Cam::NO_ERROR : Cam::DEVICE_ERROR;
    }

    void OnEvent(const cam_event &e)
    {
        switch (e.type) {
        case CAM_EV_FRAME: {
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (ring_ != nullptr && e.slot < layout_.slots) {
                    const uint8_t *frame = ring_ + static_cast<size_t>(e.slot) * layout_.slot_size;
                    for (auto &[captureId, ids] : repeating_)
                        for (int32_t id : ids) {
                            auto it = streams_.find(id);
                            if (it != streams_.end() && it->second.intent != Cam::STILL_CAPTURE)
                                WriteFrame(it->second, frame, e.timestamp);
                        }
                }
            }
            bridge_->Release(e.slot);
            break;
        }
        case CAM_EV_SHUTTER: {
            int32_t captureId, stream;
            {
                std::lock_guard<std::mutex> l(mutex_);
                captureId = stillCapture_;
                stream = stillStream_;
            }
            if (captureId >= 0 && cb_ != nullptr) cb_->OnFrameShutter(captureId, {stream}, e.timestamp);
            break;
        }
        case CAM_EV_STILL: {
            // The preview keeps running until the capture or the streams are cancelled; nothing here may wait on
            // camserver (this is the thread that reads its replies).
            int32_t captureId, stream;
            int rc = Cam::DEVICE_ERROR;
            {
                std::lock_guard<std::mutex> l(mutex_);
                captureId = stillCapture_;
                stream = stillStream_;
                auto it = streams_.find(stream);
                if (e.status == 0 && it != streams_.end() && ring_ != nullptr && e.size <= layout_.jpeg_capacity)
                    rc = WriteJpeg(it->second, captureId, ring_ + layout_.jpeg_offset, e.size, e.timestamp);
                stillCapture_ = -1;
            }
            if (captureId < 0 || cb_ == nullptr) break;
            if (cb13_ != nullptr) cb13_->OnFrameShutterEnd(captureId, {stream}, e.timestamp);
            if (rc == Cam::NO_ERROR) {
                cb_->OnCaptureEnded(captureId, {{stream, 1}});
            } else {
                cb_->OnCaptureError(captureId, {{stream, Cam::BUFFER_LOST}});
            }
            CaptureReady(captureId, stream);
            break;
        }
        case CAM_EV_STATE:
            if (onResult_) onResult_(e.timestamp, StateResult(e.slot, e.size));
            break;
        case CAM_EV_ERROR:
            printf("CAMERA_HOST camserver error %d\n", e.status);
            if (onError_) onError_();
            break;
        }
    }

    // Tells the app it may take the next photo (after OnFrameShutterEnd, at the JPEG): Huawei's Camera app waits for photoOutput's captureReady after each
    // photo ("camera is not ready or wait save photo", NOT_CAPTURE_READY), which camera_service passes on from the
    // 1.3 callback's OnCaptureReady.
    void CaptureReady(int32_t captureId, int32_t stream)
    {
        if (cb13_ == nullptr) return;
        int32_t rc = cb13_->OnCaptureReady(captureId, {stream}, static_cast<uint64_t>(NowBoottime()));
        if (rc != 0) printf("CAMERA_HOST capture %d ready: %d\n", captureId, rc);
    }

    // The focus and exposure states as camera_service's results: the modes with them (it reads the state only in the
    // AUTO and CONTINUOUS_AUTO focus modes). From the HAL's AF and AE states. Reads the focus mode unlocked: Capture
    // holds the settings' lock while it waits for camserver, whose replies this thread reads.
    std::vector<uint8_t> StateResult(uint32_t af, uint32_t ae)
    {
        auto m = std::make_shared<Camera::CameraMetadata>(8, 64);
        int32_t focus = __atomic_load_n(&settings_->focus, __ATOMIC_RELAXED);
        uint8_t focusMode = focus == CAM_FOCUS_CONTINUOUS ? OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO
            : focus == CAM_FOCUS_AUTO ? OHOS_CAMERA_FOCUS_MODE_AUTO : OHOS_CAMERA_FOCUS_MODE_LOCKED;
        uint8_t exposureMode = OHOS_CAMERA_EXPOSURE_MODE_CONTINUOUS_AUTO;
        m->addEntry(OHOS_CONTROL_FOCUS_MODE, &focusMode, 1);
        m->addEntry(OHOS_CONTROL_EXPOSURE_MODE, &exposureMode, 1);
        if (af != 0) {  // not INACTIVE
            uint8_t state = af == 1 || af == 3 ? OHOS_CAMERA_FOCUS_STATE_SCAN
                : af == 2 || af == 4 ? OHOS_CAMERA_FOCUS_STATE_FOCUSED : OHOS_CAMERA_FOCUS_STATE_UNFOCUSED;
            m->addEntry(OHOS_CONTROL_FOCUS_STATE, &state, 1);
        }
        if (ae != 0) {
            uint8_t state = ae == 1 || ae == 5 ? OHOS_CAMERA_EXPOSURE_STATE_SCAN : OHOS_CAMERA_EXPOSURE_STATE_CONVERGED;
            m->addEntry(OHOS_CONTROL_EXPOSURE_STATE, &state, 1);
        }
        std::vector<uint8_t> out;
        Camera::MetadataUtils::ConvertMetadataToVec(m, out);
        return out;
    }

public:
    std::function<void()> onError_;
    std::function<void(uint64_t, const std::vector<uint8_t> &)> onResult_;

private:
    std::shared_ptr<Bridge> bridge_;
    sptr<Cam::IStreamOperatorCallback> cb_;
    sptr<Cam13::IStreamOperatorCallback> cb13_;
    cam_settings *settings_;
    std::mutex *settingsMutex_;
    std::mutex mutex_;
    std::map<int32_t, Stream> streams_;
    std::map<int32_t, std::vector<int32_t>> repeating_;
    int32_t stillCapture_ = -1, stillStream_ = -1;
    const uint8_t *ring_ = nullptr;
    cam_layout layout_{};
    bool configured_ = false, running_ = false;
};

// --- the device ---
class CameraDevice final : public Cam13::ICameraDevice {
public:
    CameraDevice(std::shared_ptr<Bridge> bridge, const sptr<Cam::ICameraDeviceCallback> &cb, std::function<void()> closed)
        : bridge_(std::move(bridge)), cb_(cb), closed_(std::move(closed)) {}

    int32_t GetStreamOperator(const sptr<Cam::IStreamOperatorCallback> &cb, sptr<Cam::IStreamOperator> &op) override
    {
        op = MakeOperator(cb, nullptr);
        return op != nullptr ? Cam::NO_ERROR : Cam::INSUFFICIENT_RESOURCES;
    }
    int32_t GetStreamOperator_V1_1(const sptr<Cam::IStreamOperatorCallback> &cb, sptr<Cam11::IStreamOperator> &op) override
    {
        op = MakeOperator(cb, nullptr);
        return op != nullptr ? Cam::NO_ERROR : Cam::INSUFFICIENT_RESOURCES;
    }
    int32_t GetStreamOperator_V1_2(const sptr<Cam12::IStreamOperatorCallback> &cb, sptr<Cam12::IStreamOperator> &op) override
    {
        op = MakeOperator(cb, nullptr);
        return op != nullptr ? Cam::NO_ERROR : Cam::INSUFFICIENT_RESOURCES;
    }
    int32_t GetStreamOperator_V1_3(const sptr<Cam13::IStreamOperatorCallback> &cb, sptr<Cam13::IStreamOperator> &op) override
    {
        op = MakeOperator(cb, cb);
        return op != nullptr ? Cam::NO_ERROR : Cam::INSUFFICIENT_RESOURCES;
    }
    int32_t GetSecureCameraSeq(uint64_t &) override { return Cam::METHOD_NOT_SUPPORTED; }

    int32_t UpdateSettings(const std::vector<uint8_t> &settings) override
    {
        std::lock_guard<std::mutex> l(settingsMutex_);
        ApplyMetadata(settings, settings_);
        bridge_->Call(CAM_SETTINGS, {}, &settings_, sizeof(settings_));
        return Cam::NO_ERROR;
    }
    int32_t SetResultMode(Cam::ResultCallbackMode) override { return Cam::NO_ERROR; }
    int32_t GetEnabledResults(std::vector<int32_t> &results) override
    {
        results.clear();
        return Cam::NO_ERROR;
    }
    int32_t EnableResult(const std::vector<int32_t> &) override { return Cam::NO_ERROR; }
    int32_t DisableResult(const std::vector<int32_t> &) override { return Cam::NO_ERROR; }
    int32_t GetDefaultSettings(std::vector<uint8_t> &settings) override
    {
        settings.clear();
        return Cam::NO_ERROR;
    }
    int32_t GetStatus(const std::vector<uint8_t> &, std::vector<uint8_t> &metaOut) override
    {
        metaOut.clear();
        return Cam::NO_ERROR;
    }
    int32_t Reset() override { return Cam::NO_ERROR; }
    void SetClosed(std::function<void()> closed) { closed_ = std::move(closed); }

    int32_t Close() override
    {
        if (isClosed_) return Cam::NO_ERROR;
        isClosed_ = true;
        if (operator_ != nullptr) operator_->Close();
        bridge_->Call(CAM_CLOSE);
        bridge_->SetHandler(nullptr);  // the connection's end is ours, not an error to report
        bridge_->Disconnect();
        printf("CAMERA_HOST closed\n");
        if (closed_) closed_();
        return Cam::NO_ERROR;
    }

private:
    sptr<StreamOperator> MakeOperator(const sptr<Cam::IStreamOperatorCallback> &cb,
                                      const sptr<Cam13::IStreamOperatorCallback> &cb13)
    {
        if (operator_ != nullptr) {
            operator_->SetCallbacks(cb, cb13);
            return operator_;
        }
        operator_ = new StreamOperator(bridge_, cb, cb13, &settings_, &settingsMutex_);
        operator_->onError_ = [this] {
            if (cb_ != nullptr) cb_->OnError(Cam::DEVICE_DISCONNECT, 0);
        };
        operator_->onResult_ = [this](uint64_t timestamp, const std::vector<uint8_t> &result) {
            if (cb_ != nullptr) cb_->OnResult(timestamp, result);
        };
        return operator_;
    }

    std::shared_ptr<Bridge> bridge_;
    sptr<Cam::ICameraDeviceCallback> cb_;
    std::function<void()> closed_;
    sptr<StreamOperator> operator_;
    std::mutex settingsMutex_;
    cam_settings settings_ = CAM_SETTINGS_DEFAULT;
    bool isClosed_ = false;
};

// --- abilities ---
std::vector<uint8_t> Abilities(const cam_info &c)
{
    auto a = std::make_shared<Camera::CameraMetadata>(64, 32768);
    uint8_t position = c.facing ? OHOS_CAMERA_POSITION_FRONT : OHOS_CAMERA_POSITION_BACK;
    // Unspecified, which apps see as the default camera: Huawei's Camera app takes only a default-type back camera
    // on a phone (its CameraDeviceManager), as their own phones report their main one.
    uint8_t type = OHOS_CAMERA_TYPE_UNSPECIFIED;
    uint8_t connection = OHOS_CAMERA_CONNECTION_TYPE_BUILTIN;
    int32_t orientation = c.orientation;
    a->addEntry(OHOS_ABILITY_CAMERA_POSITION, &position, 1);
    a->addEntry(OHOS_ABILITY_CAMERA_TYPE, &type, 1);
    a->addEntry(OHOS_ABILITY_CAMERA_CONNECTION_TYPE, &connection, 1);
    a->addEntry(OHOS_SENSOR_ORIENTATION, &orientation, 1);
    uint8_t flash = c.flash ? OHOS_CAMERA_FLASH_TRUE : OHOS_CAMERA_FLASH_FALSE;
    a->addEntry(OHOS_ABILITY_FLASH_AVAILABLE, &flash, 1);
    std::vector<uint8_t> flashModes = {OHOS_CAMERA_FLASH_MODE_CLOSE};
    if (c.flash)
        flashModes = {OHOS_CAMERA_FLASH_MODE_CLOSE, OHOS_CAMERA_FLASH_MODE_OPEN, OHOS_CAMERA_FLASH_MODE_AUTO,
                      OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN};
    a->addEntry(OHOS_ABILITY_FLASH_MODES, flashModes.data(), flashModes.size());
    // AUTO for tapping to focus: the framework takes a focus point only outside CONTINUOUS_AUTO.
    std::vector<uint8_t> focus = {OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO, OHOS_CAMERA_FOCUS_MODE_AUTO,
                                  OHOS_CAMERA_FOCUS_MODE_LOCKED};
    std::vector<uint8_t> exposure = {OHOS_CAMERA_EXPOSURE_MODE_CONTINUOUS_AUTO, OHOS_CAMERA_EXPOSURE_MODE_AUTO};
    a->addEntry(OHOS_ABILITY_FOCUS_MODES, focus.data(), focus.size());
    a->addEntry(OHOS_ABILITY_EXPOSURE_MODES, exposure.data(), exposure.size());
    int32_t ev[2] = {c.ev_min, c.ev_max};
    a->addEntry(OHOS_ABILITY_AE_COMPENSATION_RANGE, ev, 2);
    camera_rational_t step{1, c.ev_step > 0 ? static_cast<int32_t>(1.0f / c.ev_step + 0.5f) : 3};
    a->addEntry(OHOS_ABILITY_AE_COMPENSATION_STEP, &step, 1);
    // The logical back camera's range starts below 1: its ultrawide lens.
    float zoom[2] = {c.min_zoom > 0 && c.min_zoom < 1 ? c.min_zoom : 1.0f, c.max_zoom > 1 ? c.max_zoom : 1.0f};
    a->addEntry(OHOS_ABILITY_ZOOM_RATIO_RANGE, zoom, 2);
    std::vector<int32_t> sceneZoom;
    for (int32_t mode = 0; mode <= 2; mode++)
        sceneZoom.insert(sceneZoom.end(), {mode, static_cast<int32_t>(zoom[0] * 100 + 0.5f),
                                           static_cast<int32_t>(zoom[1] * 100 + 0.5f)});
    a->addEntry(OHOS_ABILITY_SCENE_ZOOM_CAP, sceneZoom.data(), sceneZoom.size());
    std::vector<int32_t> fps;
    for (uint32_t i = 0; i < c.n_fps; i++) fps.insert(fps.end(), {c.fps[i][0], c.fps[i][1]});
    if (!fps.empty()) a->addEntry(OHOS_ABILITY_FPS_RANGES, fps.data(), fps.size());
    a->addEntry(OHOS_ABILITY_FOCAL_LENGTH, &c.focal_length, 1);
    // The 35 mm equivalent, which Huawei's Camera app needs before it opens anything (it reads the camera's
    // lensEquivalentFocalLength): focal length times the full frame's diagonal over the sensor's.
    double diagonal = std::sqrt(c.physical_width * c.physical_width + c.physical_height * c.physical_height);
    int32_t equivalent = diagonal > 0 ? static_cast<int32_t>(c.focal_length * 43.27 / diagonal + 0.5) : 26;
    a->addEntry(OHOS_ABILITY_LENS_EQUIVALENT_FOCUS, &equivalent, 1);
    std::vector<int32_t> modeEquivalent;
    for (int32_t mode = 0; mode <= 2; mode++) modeEquivalent.insert(modeEquivalent.end(), {mode, equivalent});
    a->addEntry(OHOS_ABILITY_EQUIVALENT_FOCUS, modeEquivalent.data(), modeEquivalent.size());
    int32_t active[4] = {0, 0, static_cast<int32_t>(c.active_width), static_cast<int32_t>(c.active_height)};
    a->addEntry(OHOS_SENSOR_INFO_ACTIVE_ARRAY_SIZE, active, 4);
    uint8_t mute = 0;
    // Video stabilisation (camserver: EIS with the gyro, video sessions only); AUTO is the app's "on".
    std::vector<uint8_t> stabilization = {OHOS_CAMERA_VIDEO_STABILIZATION_OFF, OHOS_CAMERA_VIDEO_STABILIZATION_AUTO};
    a->addEntry(OHOS_ABILITY_MUTE_MODES, &mute, 1);
    a->addEntry(OHOS_ABILITY_VIDEO_STABILIZATION_MODES, stabilization.data(), stabilization.size());

    // Profiles: preview sizes up to 2048 wide (what the bridge copies at 30 fps), the JPEG sizes. The preview and
    // video streams are NV21; mode 0 (normal), 1 (photo) and 2 (video) get the same.
    std::vector<int32_t> previews, stills;
    for (uint32_t i = 0; i < c.n_preview; i++) {
        const cam_size &s = c.preview[i];
        if (s.width > 2048 || s.height > 1536 || s.width < 320 || (s.width & 1) || (s.height & 1)) continue;
        previews.insert(previews.end(), {OHOS_CAMERA_FORMAT_YCRCB_420_SP, static_cast<int32_t>(s.width),
                                         static_cast<int32_t>(s.height), 30, 15, 30, -1});
    }
    for (uint32_t i = 0; i < c.n_still; i++) {
        const cam_size &s = c.still[i];
        if (s.width < 640) continue;
        stills.insert(stills.end(), {OHOS_CAMERA_FORMAT_JPEG, static_cast<int32_t>(s.width),
                                     static_cast<int32_t>(s.height), 30, 15, 30, -1});
    }
    std::vector<int32_t> extend;
    for (int32_t mode = 0; mode <= 2; mode++) {
        extend.push_back(mode);
        extend.push_back(0);  // preview
        extend.insert(extend.end(), previews.begin(), previews.end());
        extend.push_back(-1);
        extend.push_back(1);  // video (the client takes mode 0's for every mode)
        extend.insert(extend.end(), previews.begin(), previews.end());
        extend.push_back(-1);
        extend.push_back(2);  // still capture
        extend.insert(extend.end(), stills.begin(), stills.end());
        extend.push_back(-1);
        extend.push_back(-1);
    }
    a->addEntry(OHOS_ABILITY_STREAM_AVAILABLE_EXTEND_CONFIGURATIONS, extend.data(), extend.size());
    std::vector<uint8_t> out;
    Camera::MetadataUtils::ConvertMetadataToVec(a, out);
    return out;
}

// --- the host ---
class CameraHost final : public Cam13::ICameraHost {
public:
    int32_t SetCallback(const sptr<Cam::ICameraHostCallback> &callback) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = callback;
        Report();
        return Cam::NO_ERROR;
    }

    int32_t SetCallback_V1_2(const sptr<Cam12::ICameraHostCallback> &callback) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = callback;
        callback12_ = callback;
        Report();
        return Cam::NO_ERROR;
    }

    int32_t GetCameraIds(std::vector<std::string> &cameraIds) override
    {
        cameraIds.clear();
        for (uint32_t i = 0; i < Count(); i++) cameraIds.push_back(CAMERA_IDS[i]);
        return Cam::NO_ERROR;
    }

    int32_t GetCameraAbility(const std::string &cameraId, std::vector<uint8_t> &cameraAbility) override
    {
        int i = Index(cameraId);
        if (i < 0) return Cam::INVALID_ARGUMENT;
        if (g_list.count == 0) {  // no camserver: the flash only, as before
            auto ability = std::make_shared<Camera::CameraMetadata>(8, 64);
            uint8_t position = OHOS_CAMERA_POSITION_BACK, type = OHOS_CAMERA_TYPE_WIDE_ANGLE;
            uint8_t connection = OHOS_CAMERA_CONNECTION_TYPE_BUILTIN, flash = OHOS_CAMERA_FLASH_TRUE;
            ability->addEntry(OHOS_ABILITY_CAMERA_POSITION, &position, 1);
            ability->addEntry(OHOS_ABILITY_CAMERA_TYPE, &type, 1);
            ability->addEntry(OHOS_ABILITY_CAMERA_CONNECTION_TYPE, &connection, 1);
            ability->addEntry(OHOS_ABILITY_FLASH_AVAILABLE, &flash, 1);
            return Camera::MetadataUtils::ConvertMetadataToVec(ability, cameraAbility) ? Cam::NO_ERROR
                                                                                        : Cam::DEVICE_ERROR;
        }
        cameraAbility = Abilities(g_list.camera[i]);
        return cameraAbility.empty() ? Cam::DEVICE_ERROR : Cam::NO_ERROR;
    }

    int32_t OpenCamera(const std::string &id, const sptr<Cam::ICameraDeviceCallback> &cb,
                       sptr<Cam::ICameraDevice> &device) override
    {
        sptr<CameraDevice> d;
        int32_t rc = Open(id, cb, d);
        device = d;
        return rc;
    }
    int32_t OpenCamera_V1_1(const std::string &id, const sptr<Cam::ICameraDeviceCallback> &cb,
                            sptr<Cam11::ICameraDevice> &device) override
    {
        sptr<CameraDevice> d;
        int32_t rc = Open(id, cb, d);
        device = d;
        return rc;
    }
    int32_t OpenCamera_V1_2(const std::string &id, const sptr<Cam::ICameraDeviceCallback> &cb,
                            sptr<Cam12::ICameraDevice> &device) override
    {
        sptr<CameraDevice> d;
        int32_t rc = Open(id, cb, d);
        device = d;
        return rc;
    }
    int32_t OpenCamera_V1_3(const std::string &id, const sptr<Cam::ICameraDeviceCallback> &cb,
                            sptr<Cam13::ICameraDevice> &device) override
    {
        sptr<CameraDevice> d;
        int32_t rc = Open(id, cb, d);
        device = d;
        return rc;
    }
    int32_t OpenSecureCamera(const std::string &, const sptr<Cam::ICameraDeviceCallback> &,
                             sptr<Cam13::ICameraDevice> &) override
    {
        return Cam::METHOD_NOT_SUPPORTED;
    }
    int32_t GetResourceCost(const std::string &, Cam13::CameraDeviceResourceCost &) override
    {
        return Cam::METHOD_NOT_SUPPORTED;
    }

    int32_t Prelaunch(const Cam11::PrelaunchConfig &) override { return Cam::NO_ERROR; }
    int32_t PrelaunchWithOpMode(const Cam11::PrelaunchConfig &, int32_t) override { return Cam::NO_ERROR; }
    int32_t PreCameraSwitch(const std::string &) override { return Cam::NO_ERROR; }
    int32_t NotifyDeviceStateChangeInfo(int32_t, int32_t) override { return Cam::NO_ERROR; }

    int32_t SetFlashlight(const std::string &cameraId, bool isEnable) override
    {
        if (Index(cameraId) != 0) return Cam::INVALID_ARGUMENT;
        return Torch(isEnable ? 1.0f : 0.0f);
    }

    int32_t SetFlashlight_V1_2(float level) override { return Torch(level); }

private:
    static uint32_t Count() { return g_list.count ? g_list.count : 1; }
    static int Index(const std::string &id)
    {
        for (uint32_t i = 0; i < Count(); i++)
            if (id == CAMERA_IDS[i]) return static_cast<int>(i);
        return -1;
    }

    int32_t Open(const std::string &id, const sptr<Cam::ICameraDeviceCallback> &cb, sptr<CameraDevice> &device)
    {
        int i = Index(id);
        if (i < 0 || g_list.count == 0) return i < 0 ? Cam::INVALID_ARGUMENT : Cam::METHOD_NOT_SUPPORTED;
        // camera_service is our only client, so an open while a device is still open means it lost that one (it
        // restarted, or the app died before closing): close it, which also frees camserver's one connection.
        sptr<CameraDevice> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = device_;
            if (on_) {  // the HAL drives the flash while a camera is open
                SetTorch(0);
                Report();
            }
        }
        if (previous != nullptr) {
            printf("CAMERA_HOST open while a device is open: closing that one\n");
            previous->Close();
        }
        auto bridge = std::make_shared<Bridge>();
        int rc = bridge->Connect();
        if (!rc) rc = bridge->Call(CAM_OPEN, {i});
        printf("CAMERA_HOST open %s: %d\n", id.c_str(), rc);
        if (rc) return Cam::DEVICE_ERROR;
        CameraDevice *d = new CameraDevice(bridge, cb, nullptr);
        device = d;
        d->SetClosed([this, d] {
            std::lock_guard<std::mutex> lock(mutex_);
            if (device_.GetRefPtr() == d) device_ = nullptr;
        });
        std::lock_guard<std::mutex> lock(mutex_);
        device_ = device;
        return Cam::NO_ERROR;
    }

    int32_t Torch(float level)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_ != nullptr) return Cam::CAMERA_BUSY;
        bool ok = SetTorch(level);
        printf("TORCH level %.2f %s\n", level, ok ? "ok" : "failed");
        if (!ok) return Cam::DEVICE_ERROR;
        Report();
        return Cam::NO_ERROR;
    }

    // With mutex_ held.
    bool SetTorch(float level)
    {
        bool enable = level > 0.0f;
        int ma = static_cast<int>(TORCH_MA * (level > 1.0f ? 1.0f : level));
        bool ok = true;
        if (enable) {
            for (const char *led : TORCH_LEDS)
                ok = WriteValue(led, ma < TORCH_MA_MIN ? TORCH_MA_MIN : ma) && ok;
            ok = WriteValue(TORCH_SWITCH, 1) && ok;
        } else {
            ok = WriteValue(TORCH_SWITCH, 0);
            for (const char *led : TORCH_LEDS)
                ok = WriteValue(led, 0) && ok;
        }
        if (ok) on_ = enable;
        return ok;
    }

    // With mutex_ held.
    void Report()
    {
        Cam::FlashlightStatus status = on_ ? Cam::FLASHLIGHT_ON : Cam::FLASHLIGHT_OFF;
        if (callback12_ != nullptr)
            callback12_->OnFlashlightStatus_V1_2(status);
        if (callback_ != nullptr)
            callback_->OnFlashlightStatus(CAMERA_IDS[0], status);
    }

    std::mutex mutex_;
    sptr<Cam::ICameraHostCallback> callback_;
    sptr<Cam12::ICameraHostCallback> callback12_;
    sptr<CameraDevice> device_;
    bool on_ = false;
};

// The service manager's registration call, through libhdf_ipc_adapter's C API (as the vibrator and motion publishers).
struct RegistryApi {
    HdfRemoteService *(*getRemote)(int32_t) = nullptr;
    bool (*setDesc)(HdfRemoteService *, const char *) = nullptr;
    bool (*writeToken)(HdfRemoteService *, HdfSBuf *) = nullptr;
    HdfSBuf *(*obtain)(HdfSbufType) = nullptr;
    void (*recycle)(HdfSBuf *) = nullptr;
    bool (*writeString)(HdfSBuf *, const char *) = nullptr;
    bool (*writeU16)(HdfSBuf *, uint16_t) = nullptr;
    bool (*writeU32)(HdfSBuf *, uint32_t) = nullptr;
    int32_t (*toParcel)(HdfSBuf *, MessageParcel **) = nullptr;

    bool Load(void *library)
    {
#define LOAD_MEMBER(member, symbol) \
        *reinterpret_cast<void **>(&(member)) = dlsym(library, symbol); \
        if ((member) == nullptr) return false
        LOAD_MEMBER(getRemote, "HdfRemoteServiceGet");
        LOAD_MEMBER(setDesc, "HdfRemoteServiceSetInterfaceDesc");
        LOAD_MEMBER(writeToken, "HdfRemoteServiceWriteInterfaceToken");
        LOAD_MEMBER(obtain, "HdfSbufTypedObtain");
        LOAD_MEMBER(recycle, "HdfSbufRecycle");
        LOAD_MEMBER(writeString, "HdfSbufWriteString");
        LOAD_MEMBER(writeU16, "HdfSbufWriteUint16");
        LOAD_MEMBER(writeU32, "HdfSbufWriteUint32");
        LOAD_MEMBER(toParcel, "SbufToParcel");
#undef LOAD_MEMBER
        return true;
    }

    int32_t Call(HdfRemoteService *registry, int code, const sptr<IRemoteObject> &stub)
    {
        HdfSBuf *data = obtain(SBUF_IPC);
        HdfSBuf *reply = obtain(SBUF_IPC);
        MessageParcel *parcel = nullptr;
        bool valid = data != nullptr && reply != nullptr && writeToken(registry, data) &&
            writeString(data, SERVICE_NAME);
        if (valid && stub != nullptr) {  // add: class, info, the stub, an empty info string, the descriptor
            valid = writeU16(data, DEVICE_CLASS_CAMERA) && writeU32(data, 0) && toParcel(data, &parcel) == 0 &&
                parcel != nullptr && parcel->WriteRemoteObject(stub) && writeString(data, "") &&
                writeString(data, SERVICE_DESCRIPTOR);
        }
        int32_t result = valid ? registry->dispatcher->Dispatch(registry, code, data, reply) : HDF_FAILURE;
        if (data != nullptr) recycle(data);
        if (reply != nullptr) recycle(reply);
        return result;
    }
};

// The cameras camserver offers; it starts with the Android helpers, so wait for it a while.
void LoadCameras(int seconds)
{
    for (int t = 0; t < seconds * 2; t++) {
        Bridge b;
        if (b.Connect() == 0) {
            static cam_list l;
            int rc = b.Call(CAM_LIST, {}, nullptr, 0, &l, sizeof(l));
            b.Disconnect();
            if (rc == 0 && l.count > 0) {
                g_list = l;
                if (g_list.count > 4) g_list.count = 4;
                printf("CAMERA_HOST %u cameras from camserver\n", g_list.count);
                return;
            }
        }
        usleep(500000);
    }
    printf("CAMERA_HOST no camserver: the flash only\n");
}
}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (sem_init(&stopWake, 0, 0) != 0) return 12;
    struct sigaction action{};
    action.sa_handler = StopSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    auto serviceManager = HDI::ServiceManager::V1_0::IServiceManager::Get();
    if (serviceManager == nullptr) {
        puts("CAMERA_PUBLISH_REGISTRY_AVAILABLE=0");
        return 1;
    }
    if (serviceManager->GetService(SERVICE_NAME) != nullptr) {
        puts("CAMERA_PUBLISH_ALREADY_REGISTERED=1");
        return 2;
    }
    // The stub registers its factory with the object collector when loaded; the proxy turns the callbacks camera_service
    // passes into objects.
    void *stubs = dlopen("/vendor/lib64/libcamera_stub_1.3.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *proxies = dlopen("libcamera_proxy_1.3.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *ipc = dlopen("libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_GLOBAL);
    printf("CAMERA_PUBLISH_LOAD stubs=%d proxies=%d ipc=%d\n", stubs != nullptr, proxies != nullptr,
        ipc != nullptr);
    RegistryApi api;
    if (stubs == nullptr || proxies == nullptr || ipc == nullptr || !api.Load(ipc)) {
        printf("CAMERA_PUBLISH_DLERROR %s\n", dlerror());
        return 3;
    }
    if (!surfaces.Load()) return 4;
    LoadCameras(60);
    sptr<Cam13::ICameraHost> implementation = new CameraHost();
    auto stub = HDI::ObjectCollector::GetInstance().GetOrNewObject(implementation, Cam13::ICameraHost::GetDescriptor());
    if (stub == nullptr) return 6;
    auto registry = api.getRemote(5100);
    if (registry == nullptr || !api.setDesc(registry, "HDI.IServiceManager.V1_0")) return 7;
    int32_t published = api.Call(registry, 1, stub);
    printf("CAMERA_PUBLISH_RESULT rc=%d uid=%u\n", published, getuid());
    if (published != HDF_SUCCESS) return 8;
    while (sem_wait(&stopWake) != 0 && errno == EINTR) {}
    int32_t removed = api.Call(registry, 8, nullptr);
    printf("CAMERA_PUBLISH_CLEANUP unpublish=%d\n", removed);
    _exit(removed == HDF_SUCCESS ? 0 : 9);
}
