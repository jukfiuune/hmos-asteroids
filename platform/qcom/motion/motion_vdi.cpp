/* SPDX-License-Identifier: MIT
 * asteroids: the motion VDI (/vendor/lib64/libhdi_motion_impl.z.so) under the donor's own motion service
 * (libmotion_interface_service_1.1.z.so's MotionIfService, which loads it by this name). Huawei's VDI reads motions
 * from their sensor hub; this one computes the one the window manager needs, screen rotation, from the
 * accelerometer through HarmonyOS's public sensor API. Other motion types are accepted and never reported.
 *
 * Rotation (as Android's WindowOrientationListener, simplified): the accelerometer is low-pass filtered; readings
 * far from 1 g (the phone is being moved) or with the phone near flat are ignored; the tilt angle in the screen
 * plane picks the nearest quarter turn once it is within 30 degrees of it (15 degrees of hysteresis either side of
 * each boundary) and has held for 300 ms. Reported as status (and data[0]) 0..3: upright, then quarter turns.
 * A worker thread does everything that can block, outside the lock: subscribing takes seconds, longer than the
 * service's watchdog allows a call (XCollie killed it after 8 s in EnableMotion), and reports go out from there
 * too. A report sent from inside EnableMotion deadlocked foundation: its sensor queue waits in msdp's subscribe,
 * which waits for EnableMotion, while the report needs that same queue (its watchdog killed foundation, and the
 * whole UI restarted).
 * For testing, the parameter asteroids.motion.force_rotation (0..3) overrides the accelerometer while rotation is
 * enabled ("param set asteroids.motion.force_rotation 1"; an empty value hands back to the sensor). */
#include "imotion_interface_vdi.h"

#include <sensors/oh_sensor.h>

#include <hilog/log.h>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern "C" int GetParameter(const char *key, const char *def, char *value, unsigned int len);

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002516
#define LOG_TAG "asteroids_motion_vdi"
#define LOGI(...) HILOG_INFO(LOG_CORE, __VA_ARGS__)
#define LOGE(...) HILOG_ERROR(LOG_CORE, __VA_ARGS__)

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_1 {
namespace {
constexpr int32_t ROTATION = 4;  // HDF_MOTION_TYPE_ROTATION
constexpr int64_t INTERVAL_NS = 50000000;
constexpr float ALPHA = 0.25f;  // low-pass weight per 50 ms sample
constexpr float GRAVITY = 9.80665f;
constexpr float FLAT_RATIO = 0.85f;  // |z| above this share of the reading: lying flat, keep the rotation
constexpr double CAPTURE_DEG = 30.0;
constexpr int64_t SETTLE_NS = 300000000;

int64_t Now()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

class Motion : public IMotionInterfaceVdi {
public:
    static Motion &Get()
    {
        static Motion motion;
        return motion;
    }

    int32_t InitMotion() override
    {
        LOGI("init");
        return HDF_SUCCESS;
    }

    int32_t EnableMotion(int32_t motionType) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        LOGI("enable %{public}d", motionType);
        enabled_.insert(motionType);
        if (motionType == ROTATION) {
            StartWorkerLocked();
            reportPending_ = rotation_ >= 0;  // the current rotation, for the new subscriber
            wake_.notify_all();
        }
        return HDF_SUCCESS;
    }

    int32_t DisableMotion(int32_t motionType) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        LOGI("disable %{public}d", motionType);
        enabled_.erase(motionType);
        if (motionType == ROTATION) wake_.notify_all();
        return HDF_SUCCESS;
    }

    int32_t RegisterMotionCallback(const sptr<IMotionCallbackVdi> cb) override
    {
        if (cb == nullptr) return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &have : callbacks_) {
            if (have->HandleCallbackDeath() == cb->HandleCallbackDeath()) return HDF_SUCCESS;
        }
        callbacks_.push_back(cb);
        LOGI("callbacks %{public}zu", callbacks_.size());
        return HDF_SUCCESS;
    }

    int32_t UnregisterMotionCallback(const sptr<IMotionCallbackVdi> cb) override
    {
        if (cb == nullptr) return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lock(mutex_);
        // The service wraps each call's callback anew: compare the remote objects behind them.
        for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
            if ((*it)->HandleCallbackDeath() == cb->HandleCallbackDeath()) {
                callbacks_.erase(it);
                break;
            }
        }
        LOGI("callbacks %{public}zu", callbacks_.size());
        return HDF_SUCCESS;
    }

    int32_t SetMotionConfig(int32_t motionType, const std::vector<uint8_t> &data) override
    {
        LOGI("config %{public}d (%{public}zu bytes)", motionType, data.size());
        return HDF_SUCCESS;
    }

private:
    static void OnEvent(Sensor_Event *event)
    {
        float *data = nullptr;
        uint32_t length = 0;
        if (OH_SensorEvent_GetData(event, &data, &length) == SENSOR_SUCCESS && data != nullptr && length >= 3) {
            Get().Sample(data[0], data[1], data[2]);
        }
    }

    void StartWorkerLocked()
    {
        if (worker_.joinable()) return;
        worker_ = std::thread([this] { Work(); });
    }

    // Subscribes while rotation is enabled and sends reports, outside the lock (Unsubscribe may wait for a callback
    // in Sample, and a report is an IPC call into msdp).
    void Work()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            bool want = enabled_.count(ROTATION) != 0;
            bool have = subscriber_ != nullptr;
            if (reportPending_) {
                reportPending_ = false;
                if (!want || rotation_ < 0) continue;
                HdfMotionEventVdi event{ROTATION, 0, rotation_, 1, {rotation_}};
                std::vector<sptr<IMotionCallbackVdi>> callbacks = callbacks_;
                lock.unlock();
                for (const auto &cb : callbacks) cb->OnDataEventVdi(event);
                lock.lock();
                continue;
            }
            if (want == have) {
                wake_.wait(lock);
                continue;
            }
            if (want) {
                Sensor_SubscriptionId *id = OH_Sensor_CreateSubscriptionId();
                Sensor_SubscriptionAttribute *attribute = OH_Sensor_CreateSubscriptionAttribute();
                Sensor_Subscriber *subscriber = OH_Sensor_CreateSubscriber();
                OH_SensorSubscriptionId_SetType(id, SENSOR_TYPE_ACCELEROMETER);
                OH_SensorSubscriptionAttribute_SetSamplingInterval(attribute, INTERVAL_NS);
                OH_SensorSubscriber_SetCallback(subscriber, OnEvent);
                filtered_ = false;
                candidate_ = -1;
                id_ = id, attribute_ = attribute, subscriber_ = subscriber;
                lock.unlock();
                Sensor_Result result = OH_Sensor_Subscribe(id, attribute, subscriber);
                LOGI("accelerometer subscribe %{public}d", result);
                lock.lock();
                if (result != SENSOR_SUCCESS) {
                    ReleaseLocked();
                    wake_.wait_for(lock, std::chrono::seconds(5));  // the sensors service may still be coming up
                }
            } else {
                Sensor_SubscriptionId *id = id_;
                Sensor_Subscriber *subscriber = subscriber_;
                lock.unlock();
                LOGI("accelerometer unsubscribe %{public}d", OH_Sensor_Unsubscribe(id, subscriber));
                lock.lock();
                ReleaseLocked();
            }
        }
    }

    void ReleaseLocked()
    {
        OH_Sensor_DestroySubscriber(subscriber_);
        OH_Sensor_DestroySubscriptionAttribute(attribute_);
        OH_Sensor_DestroySubscriptionId(id_);
        subscriber_ = nullptr;
        attribute_ = nullptr;
        id_ = nullptr;
    }

    void Sample(float x, float y, float z)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (subscriber_ == nullptr) return;
        if (++samples_ % 10 == 0) {  // twice a second at 50 ms
            char forced[4] = {0};
            int n = GetParameter("asteroids.motion.force_rotation", "", forced, sizeof(forced));
            forced_ = n == 1 && forced[0] >= '0' && forced[0] <= '3' ? forced[0] - '0' : -1;
        }
        if (forced_ >= 0) {
            if (forced_ != rotation_) {
                rotation_ = forced_;
                LOGI("rotation %{public}d (forced)", rotation_);
                reportPending_ = true;
                wake_.notify_all();
            }
            return;
        }
        if (!filtered_) {
            fx_ = x, fy_ = y, fz_ = z;
            filtered_ = true;
        } else {
            fx_ += ALPHA * (x - fx_), fy_ += ALPHA * (y - fy_), fz_ += ALPHA * (z - fz_);
        }
        float magnitude = std::sqrt(fx_ * fx_ + fy_ * fy_ + fz_ * fz_);
        if (magnitude < 0.5f * GRAVITY || magnitude > 1.5f * GRAVITY) return;
        if (std::fabs(fz_) > FLAT_RATIO * magnitude) return;
        // 0 degrees: upright; growing as the phone turns counterclockwise (its right edge rising).
        double angle = std::atan2(fx_, fy_) * 180.0 / M_PI;
        if (angle < 0) angle += 360.0;
        int nearest = static_cast<int>(std::lround(angle / 90.0)) % 4;
        double off = std::fabs(angle - nearest * 90.0);
        if (off > 180.0) off = 360.0 - off;
        if (off > CAPTURE_DEG || nearest == rotation_) {
            candidate_ = -1;
            return;
        }
        int64_t now = Now();
        if (nearest != candidate_) {
            candidate_ = nearest;
            since_ = now;
            return;
        }
        if (now - since_ < SETTLE_NS) return;
        rotation_ = nearest;
        candidate_ = -1;
        LOGI("rotation %{public}d (angle %{public}.0f)", rotation_, angle);
        reportPending_ = true;
        wake_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::set<int32_t> enabled_;
    std::vector<sptr<IMotionCallbackVdi>> callbacks_;
    Sensor_SubscriptionId *id_ = nullptr;
    Sensor_SubscriptionAttribute *attribute_ = nullptr;
    Sensor_Subscriber *subscriber_ = nullptr;
    bool filtered_ = false;
    float fx_ = 0, fy_ = 0, fz_ = 0;
    int rotation_ = -1;
    bool reportPending_ = false;
    unsigned samples_ = 0;
    int forced_ = -1;
    int candidate_ = -1;
    int64_t since_ = 0;
};

int CreateInstance(struct HdfVdiBase *base)
{
    reinterpret_cast<WrapperMotionVdi *>(base)->motionModule = &Motion::Get();
    return HDF_SUCCESS;
}

int DestroyInstance(struct HdfVdiBase *base)
{
    reinterpret_cast<WrapperMotionVdi *>(base)->motionModule = nullptr;
    return HDF_SUCCESS;
}

WrapperMotionVdi g_wrapper = {{1, "motion_service", CreateInstance, DestroyInstance}, nullptr};
}  // namespace
}  // namespace V1_1
}  // namespace Motion
}  // namespace HDI
}  // namespace OHOS

extern "C" {
__attribute__((visibility("default"))) HDF_VDI_INIT(OHOS::HDI::Motion::V1_1::g_wrapper);
}
