/* SPDX-License-Identifier: MIT
 * asteroids: the vibrator VDI (/vendor/lib64/libhdi_vibrator_impl.z.so) under the donor's own vibrator service
 * (libvibrator_interface_service_2.0.z.so, which loads it by this name). Huawei's VDI drives their haptic chip through
 * sysfs files and their haptics engine; this one plays everything on the Richtek motor through Android's RichTap engine
 * (the RichTap server, richtap_server.c), as LineageOS/EvoX's nothing-rt_ics HAL does: one-shots as its "on", preset
 * effects ("haptic.clock.timer", "haptic.long_press.medium", ...) as its click or tick picked from the effect's name,
 * composite effects and HD-haptics patterns as a sequence of those. Until the server is up (it starts with the Android
 * root, after this), the same pulses go to the chip's RAM sine through force feedback (haptic_ff.c).
 * No HD haptics (GetHapticCapacity says so), so the system uses presets and one-shots.
 * The donor's service calls the methods without a device argument (Huawei's VDI overrides only those); the ones
 * with one (OpenHarmony's other build of the service) forward to them. The vtable layout is drivers_peripheral's
 * ivibrator_interface_vdi.h (unchanged since 2025-07), which matches the donor's: 37 entries, checked against
 * Huawei's VibratorImpl. */
#include "ivibrator_interface_vdi.h"
#include "haptic_ff.h"
#include "richtap_client.h"

#include <hilog/log.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002519
#define LOG_TAG "asteroids_vibrator_vdi"
#define LOGI(...) HILOG_INFO(LOG_CORE, __VA_ARGS__)
#define LOGE(...) HILOG_ERROR(LOG_CORE, __VA_ARGS__)

namespace OHOS {
namespace HDI {
namespace Vibrator {
namespace V1_1 {
namespace {
using Dev = OHOS::HDI::Vibrator::V2_0::DeviceVibratorInfo;
using Clock = std::chrono::steady_clock;

// The engine's effects take this long (nothing-rt_ics's PRIMITIVE_CLICK_MS and PRIMITIVE_TICK_MS).
constexpr uint32_t CLICK_MS = 18;
constexpr uint32_t TICK_MS = 12;

struct Pulse {
    int32_t at;        // ms from the start of the sequence
    uint32_t ms;       // the vibration's length (an effect's own)
    uint32_t strength; // 1..100
    int32_t effect;    // RICHTAP_EFFECT_CLICK or _TICK, or 0 to vibrate for ms
};

Pulse Click(int32_t at, uint32_t strength) { return {at, CLICK_MS, strength, RICHTAP_EFFECT_CLICK}; }

// A preset effect as a pulse: system effects are named by feel ("haptic.clock.timer" for a picker's detent,
// "haptic.long_press.heavy", "haptic.notice.success", ...). The light ones are the engine's tick, notices a short
// vibration, everything else its click (Android's click, heavy click, thud and pop all are, in nothing-rt_ics).
Pulse PresetPulse(const std::string &effect)
{
    auto has = [&effect](const char *word) { return effect.find(word) != std::string::npos; };
    if (has("notice") || has("success") || has("fail") || has("warning"))
        return {0, 40, 100, 0};
    if (has("clock") || has("slide") || has("threshold") || has("soft") || has("light"))
        return {0, TICK_MS, 100, RICHTAP_EFFECT_TICK};
    return Click(0, 100);
}

// nothing-rt_ics's scaleToAmplitude: the engine's amplitude (0..255) from a 0..1 scale, with its gamma.
int32_t Amplitude(uint32_t strength)
{
    int32_t a = static_cast<int32_t>(std::lround(std::pow(strength / 100.0f, 1.2f) * 0xff));
    return a < 2 ? 2 : (a > 0xff ? 0xff : a);
}

class Motor final : public IVibratorInterfaceVdi {
public:
    static Motor &Get()
    {
        static Motor motor;
        return motor;
    }

    int32_t Init() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ < 0)
            fd_ = HapticOpen();
        if (fd_ < 0) {
            LOGE("no haptic_rt input device");
            return HDF_FAILURE;
        }
        if (!worker_.joinable())
            worker_ = std::thread([this] { Run(); });
        LOGI("haptic_rt open");
        return HDF_SUCCESS;
    }

    int32_t StartOnce(uint32_t duration) override
    {
        return Play({{0, duration, 100, 0}});
    }

    int32_t Start(const std::string &effectType) override
    {
        return Play({PresetPulse(effectType)});
    }

    int32_t StartByIntensity(const std::string &effectType, uint16_t intensity) override
    {
        Pulse p = PresetPulse(effectType);
        if (intensity > 0)
            p.strength = intensity > 100 ? 100 : intensity;
        return Play({p});
    }

    int32_t Stop(HdfVibratorModeVdi) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        generation_++;
        until_ = Clock::now();
        if (RichtapRequest(richtap_, RICHTAP_STOP, 0, 0) == -EPIPE)
            HapticStop(fd_);
        return HDF_SUCCESS;
    }

    int32_t GetVibratorInfo(std::vector<HdfVibratorInfoVdi> &vibratorInfo) override
    {
        HdfVibratorInfoVdi info{};
        info.isSupportIntensity = true;
        info.isSupportFrequency = false;
        info.intensityMaxValue = 100;
        info.intensityMinValue = 1;
        info.deviceId = -1;
        info.vibratorId = 1;
        info.position = 0;
        info.isLocal = 1;
        vibratorInfo.assign(1, info);
        return HDF_SUCCESS;
    }

    int32_t GetDeviceVibratorInfo(std::vector<HdfVibratorInfoVdi> &vibratorInfo) override
    {
        return GetVibratorInfo(vibratorInfo);
    }

    int32_t EnableVibratorModulation(uint32_t duration, uint16_t intensity, int16_t) override
    {
        return Play({{0, duration, intensity == 0 ? 100u : (intensity > 100 ? 100u : intensity), 0}});
    }

    int32_t EnableCompositeEffect(const HdfCompositeEffectVdi &effect) override
    {
        std::vector<Pulse> pulses;
        int32_t at = 0;
        for (const HdfEffectVdi &e : effect.effects) {
            if (effect.type == VDI_EFFECT_TYPE_TIME) {
                at += e.timeEffect.delay;
                uint32_t strength = e.timeEffect.intensity == 0 ? 100 : e.timeEffect.intensity;
                pulses.push_back({at, static_cast<uint32_t>(e.timeEffect.time), strength > 100 ? 100 : strength, 0});
                at += e.timeEffect.time;
            } else {
                at += e.primitiveEffect.delay;
                uint32_t strength = e.primitiveEffect.intensity == 0 ? 75 : e.primitiveEffect.intensity;
                pulses.push_back(Click(at, strength > 100 ? 100 : strength));
                at += CLICK_MS;
            }
        }
        return Play(pulses);
    }

    int32_t GetEffectInfo(const std::string &effectType, HdfEffectInfoVdi &effectInfo) override
    {
        effectInfo.isSupportEffect = true;
        effectInfo.duration = static_cast<int32_t>(PresetPulse(effectType).ms);
        return HDF_SUCCESS;
    }

    int32_t IsVibratorRunning(bool &state) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = !pending_.empty() || Clock::now() < until_;
        return HDF_SUCCESS;
    }

    // HD haptics patterns: each event as a pulse, transients as clicks, continuous ones for their duration.
    int32_t PlayHapticPattern(const HapticPaketVdi &pkg) override
    {
        std::vector<Pulse> pulses;
        for (const HapticEventVdi &e : pkg.events) {
            uint32_t strength = e.intensity <= 0 ? 60 : (e.intensity > 100 ? 100 : static_cast<uint32_t>(e.intensity));
            if (e.type == VDI_TRANSIENT || e.duration <= 0)
                pulses.push_back(Click(e.time, strength));
            else
                pulses.push_back({e.time, static_cast<uint32_t>(e.duration), strength, 0});
        }
        return Play(pulses);
    }

    int32_t GetHapticCapacity(HapticCapacityVdi &hapticCapacity) override
    {
        hapticCapacity = {};
        hapticCapacity.vibratorId = 1;
        return HDF_SUCCESS;
    }

    int32_t GetHapticStartUpTime(int32_t, int32_t &startUpTime) override
    {
        startUpTime = 0;
        return HDF_SUCCESS;
    }

    int32_t GetAllWaveInfo(int32_t, std::vector<OHOS::HDI::Vibrator::V2_0::HdfWaveInformation> &info) override
    {
        info.clear();
        return HDF_SUCCESS;
    }

    // The same calls with a device argument: one local motor, so it is ignored.
    int32_t StartOnce(const Dev &, uint32_t duration) override { return StartOnce(duration); }
    int32_t Start(const Dev &, const std::string &effectType) override { return Start(effectType); }
    int32_t Stop(const Dev &, HdfVibratorModeVdi mode) override { return Stop(mode); }
    int32_t EnableVibratorModulation(const Dev &, uint32_t duration, uint16_t intensity, int16_t frequency) override
    {
        return EnableVibratorModulation(duration, intensity, frequency);
    }
    int32_t EnableCompositeEffect(const Dev &, const HdfCompositeEffectVdi &effect) override
    {
        return EnableCompositeEffect(effect);
    }
    int32_t GetEffectInfo(const Dev &, const std::string &effectType, HdfEffectInfoVdi &effectInfo) override
    {
        return GetEffectInfo(effectType, effectInfo);
    }
    int32_t IsVibratorRunning(const Dev &, bool &state) override { return IsVibratorRunning(state); }
    int32_t PlayHapticPattern(const Dev &, const HapticPaketVdi &pkg) override { return PlayHapticPattern(pkg); }
    int32_t GetHapticCapacity(const Dev &, HapticCapacityVdi &hapticCapacity) override
    {
        return GetHapticCapacity(hapticCapacity);
    }
    int32_t GetHapticStartUpTime(const Dev &, int32_t mode, int32_t &startUpTime) override
    {
        return GetHapticStartUpTime(mode, startUpTime);
    }
    int32_t StartByIntensity(const Dev &, const std::string &effectType, uint16_t intensity) override
    {
        return StartByIntensity(effectType, intensity);
    }
    int32_t GetAllWaveInfo(const Dev &, std::vector<OHOS::HDI::Vibrator::V2_0::HdfWaveInformation> &info) override
    {
        return GetAllWaveInfo(0, info);
    }
    int32_t GetDeviceVibratorInfo(const Dev &, std::vector<HdfVibratorInfoVdi> &vibratorInfo) override
    {
        return GetVibratorInfo(vibratorInfo);
    }
    int32_t PlayPatternBySessionId(const Dev &, uint32_t, const HapticPaketVdi &pkg) override
    {
        return PlayHapticPattern(pkg);
    }
    int32_t StopVibrateBySessionId(const Dev &, uint32_t) override { return Stop(VDI_VIBRATOR_MODE_ONCE); }

private:
    Motor() = default;

    // Replaces whatever is playing or queued; the first pulse starts now, the rest from the worker.
    int32_t Play(const std::vector<Pulse> &pulses)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ < 0)
            return HDF_FAILURE;
        pending_.clear();
        generation_++;
        Clock::time_point start = Clock::now();
        until_ = start;
        int32_t rc = HDF_SUCCESS;
        for (const Pulse &p : pulses) {
            if (p.ms == 0)
                continue;
            Clock::time_point when = start + std::chrono::milliseconds(p.at > 0 ? p.at : 0);
            Clock::time_point end = when + std::chrono::milliseconds(p.ms);
            if (end > until_)
                until_ = end;
            if (when <= start && pending_.empty()) {
                int err = Emit(p);
                if (err != 0) {
                    LOGE("play %{public}u ms: %{public}d", p.ms, err);
                    rc = HDF_FAILURE;
                }
            } else {
                pending_.push_back({when, p});
            }
        }
        if (!pending_.empty()) {
            std::sort(pending_.begin(), pending_.end(),
                [](const std::pair<Clock::time_point, Pulse> &a, const std::pair<Clock::time_point, Pulse> &b) {
                    return a.first < b.first;
                });
            wake_.notify_one();
        }
        return rc;
    }

    void Run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (pending_.empty()) {
                wake_.wait(lock);
                continue;
            }
            uint64_t generation = generation_;
            Clock::time_point when = pending_.front().first;
            if (wake_.wait_until(lock, when, [&] { return generation_ != generation; }))
                continue;  // replaced or stopped while waiting
            Pulse p = pending_.front().second;
            pending_.erase(pending_.begin());
            Emit(p);
        }
    }

    // Through the RichTap server, connecting (again) when it isn't; force feedback while it isn't there.
    int Emit(const Pulse &p)
    {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (richtap_ < 0) {
                richtap_ = RichtapConnect();
                if (richtap_ < 0)
                    break;
                LOGI("RichTap server connected");
            }
            int32_t ret = p.effect != 0 ? RichtapRequest(richtap_, RICHTAP_EFFECT, p.effect, Amplitude(p.strength))
                                        : RichtapRequest(richtap_, RICHTAP_ON, static_cast<int32_t>(p.ms),
                                              Amplitude(p.strength));
            if (ret != -EPIPE)
                return ret < 0 ? ret : 0;
            close(richtap_);
            richtap_ = -1;
        }
        return HapticPlay(fd_, p.ms, p.strength);
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::vector<std::pair<Clock::time_point, Pulse>> pending_;
    uint64_t generation_ = 0;
    Clock::time_point until_;
    int fd_ = -1;
    int richtap_ = -1;
};

int32_t CreateInstance(struct HdfVdiBase *base)
{
    reinterpret_cast<VdiWrapperVibrator *>(base)->vibratorModule = &Motor::Get();
    return HDF_SUCCESS;
}

int32_t DestroyInstance(struct HdfVdiBase *base)
{
    reinterpret_cast<VdiWrapperVibrator *>(base)->vibratorModule = nullptr;
    return HDF_SUCCESS;
}

VdiWrapperVibrator g_wrapper = {{1, "vibrator_service", CreateInstance, DestroyInstance}, nullptr};
}  // namespace
}  // namespace V1_1
}  // namespace Vibrator
}  // namespace HDI
}  // namespace OHOS

extern "C" {
__attribute__((visibility("default"))) HDF_VDI_INIT(OHOS::HDI::Vibrator::V1_1::g_wrapper);
}
