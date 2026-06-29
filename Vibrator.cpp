/*
 * AIDL IVibrator HAL for qcom-hv-haptics via Linux input force-feedback.
 * See Vibrator.h for design notes.
 */
#define LOG_TAG "vibrator-ffinput"

#include "Vibrator.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace aidl::android::hardware::vibrator {

// Perceptibility floors: this LRA needs a little time/level to actually move,
// otherwise very short/low taps (e.g. Gboard keypress) are silent.
static constexpr int32_t kMinDurationMs = 10;
static constexpr float kMinPulseAmplitude = 0.35f;

// Predefined effect tuning (duration ms).
static constexpr int32_t kClickDurationMs = 18;
static constexpr int32_t kTickDurationMs = 12;
static constexpr int32_t kDoubleClickGapMs = 90;
static constexpr int32_t kHeavyClickDurationMs = 28;

static constexpr char kDeviceProp[] = "persist.vendor.vibrator.ffinput.device";

static bool ffBitSet(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >>
            (bit % (8 * sizeof(unsigned long)))) &
           1UL;
}

Vibrator::Vibrator() {
    if (!openDevice()) {
        LOG(ERROR) << "No qcom-hv-haptics force-feedback device found; vibration disabled";
    } else {
        LOG(INFO) << "Using haptics FF device: " << mDevicePath;
    }
}

Vibrator::~Vibrator() {
    cancelSequence();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        stopLocked();
        cancelCallbackLocked();
        if (mFd >= 0) {
            close(mFd);
            mFd = -1;
        }
    }
    if (mCallbackThread.joinable()) mCallbackThread.join();
}

bool Vibrator::openDevice() {
    std::string forced = ::android::base::GetProperty(kDeviceProp, "");
    std::vector<std::string> candidates;
    if (!forced.empty()) candidates.push_back(forced);

    DIR* dir = opendir("/dev/input");
    if (dir != nullptr) {
        std::vector<std::string> ffMatches, ffAny;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (strncmp(ent->d_name, "event", 5) != 0) continue;
            std::string path = std::string("/dev/input/") + ent->d_name;
            int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned long ffBits[1 + FF_MAX / (8 * sizeof(unsigned long))] = {0};
            char name[128] = {0};
            bool hasConstant = false;
            if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) >= 0)
                hasConstant = ffBitSet(ffBits, FF_CONSTANT);
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            close(fd);
            if (!hasConstant) continue;
            std::string lname(name);
            std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
            if (lname.find("haptic") != std::string::npos ||
                lname.find("vibrator") != std::string::npos)
                ffMatches.push_back(path);
            else
                ffAny.push_back(path);
        }
        closedir(dir);
        candidates.insert(candidates.end(), ffMatches.begin(), ffMatches.end());
        candidates.insert(candidates.end(), ffAny.begin(), ffAny.end());
    }

    for (const auto& path : candidates) {
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long ffBits[1 + FF_MAX / (8 * sizeof(unsigned long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) < 0 ||
            !ffBitSet(ffBits, FF_CONSTANT)) {
            close(fd);
            continue;
        }
        mFd = fd;
        mDevicePath = path;
        return true;
    }
    return false;
}

int16_t Vibrator::scaleMagnitude(float amplitude) {
    if (amplitude <= 0.0f) amplitude = 1.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;
    int32_t mag = static_cast<int32_t>(amplitude * 0x7fff + 0.5f);
    if (mag < 1) mag = 1;
    if (mag > 0x7fff) mag = 0x7fff;
    return static_cast<int16_t>(mag);
}

bool Vibrator::playConstantLocked(int32_t durationMs) {
    if (mFd < 0) return false;
    if (durationMs < kMinDurationMs) durationMs = kMinDurationMs;  // ensure the LRA moves

    struct ff_effect effect;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_CONSTANT;
    effect.id = mEffectId;
    effect.u.constant.level = scaleMagnitude(mAmplitude);
    effect.replay.length = static_cast<uint16_t>(std::min<int32_t>(durationMs, 0xffff));
    effect.replay.delay = 0;

    if (ioctl(mFd, EVIOCSFF, &effect) < 0) {
        LOG(ERROR) << "EVIOCSFF failed: " << strerror(errno);
        return false;
    }
    mEffectId = effect.id;

    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = mEffectId;
    play.value = 1;
    if (write(mFd, &play, sizeof(play)) != (ssize_t)sizeof(play)) {
        LOG(ERROR) << "play write failed: " << strerror(errno);
        return false;
    }
    mIsOn = true;
    mDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    return true;
}

void Vibrator::stopLocked() {
    if (mFd < 0) return;
    if (mIsOn && mEffectId >= 0) {
        struct input_event stop;
        memset(&stop, 0, sizeof(stop));
        stop.type = EV_FF;
        stop.code = mEffectId;
        stop.value = 0;
        (void)write(mFd, &stop, sizeof(stop));
    }
    if (mEffectId >= 0) {
        (void)ioctl(mFd, EVIOCRMFF, mEffectId);
        mEffectId = -1;
    }
    mIsOn = false;
}

// ---- on() completion timer ------------------------------------------------

void Vibrator::cancelCallbackLocked() {
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mCallbackCancel = true;
    mCallbackPending = false;
    mCallbackCv.notify_all();
}

void Vibrator::armCallbackLocked(int32_t durationMs,
                                 const std::shared_ptr<IVibratorCallback>& callback) {
    if (callback == nullptr) return;
    cancelCallbackLocked();
    if (mCallbackThread.joinable()) mCallbackThread.join();
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mCallbackCancel = false;
        mCallbackPending = true;
    }
    mCallbackThread = std::thread([this, durationMs, callback]() {
        std::unique_lock<std::mutex> lock(mCallbackMutex);
        if (mCallbackCv.wait_for(lock, std::chrono::milliseconds(durationMs),
                                 [this]() { return mCallbackCancel; }))
            return;
        if (!mCallbackPending) return;
        mCallbackPending = false;
        lock.unlock();
        auto ret = callback->onComplete();
        if (!ret.isOk()) LOG(WARNING) << "onComplete callback failed";
    });
}

// ---- perform()/compose() sequencing engine --------------------------------

bool Vibrator::sequenceCancelled() {
    std::lock_guard<std::mutex> lock(mWorkerMutex);
    return mWorkerCancel;
}

bool Vibrator::interruptibleSleepMs(int32_t ms) {
    if (ms <= 0) return sequenceCancelled();
    std::unique_lock<std::mutex> lock(mWorkerMutex);
    return mWorkerCv.wait_for(lock, std::chrono::milliseconds(ms),
                              [this]() { return mWorkerCancel; });
}

void Vibrator::cancelSequence() {
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mWorkerCancel = true;
    }
    mWorkerCv.notify_all();
    if (mWorker.joinable()) mWorker.join();
}

void Vibrator::startSequence(std::vector<Step> steps,
                             const std::shared_ptr<IVibratorCallback>& callback) {
    cancelSequence();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        stopLocked();
        cancelCallbackLocked();
    }
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mWorkerCancel = false;
    }
    mWorker = std::thread(&Vibrator::runSequence, this, std::move(steps), callback);
}

void Vibrator::runSequence(std::vector<Step> steps,
                           std::shared_ptr<IVibratorCallback> callback) {
    for (const auto& step : steps) {
        if (interruptibleSleepMs(step.delayMs)) goto cancelled;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mAmplitude = std::max(step.amplitude, kMinPulseAmplitude);
            playConstantLocked(step.durationMs);
        }
        bool cancel = interruptibleSleepMs(std::max(step.durationMs, kMinDurationMs));
        {
            std::lock_guard<std::mutex> lock(mMutex);
            stopLocked();
        }
        if (cancel) goto cancelled;
    }
    if (callback != nullptr) {
        auto ret = callback->onComplete();
        if (!ret.isOk()) LOG(WARNING) << "onComplete callback failed";
    }
    return;
cancelled:
    return;  // off()/new request will deliver no completion (matches AOSP semantics)
}

// ---- primitive tuning -----------------------------------------------------

float Vibrator::primitiveAmplitude(CompositePrimitive p) {
    switch (p) {
        case CompositePrimitive::CLICK:      return 0.85f;
        case CompositePrimitive::THUD:       return 1.0f;
        case CompositePrimitive::SPIN:       return 0.80f;
        case CompositePrimitive::QUICK_RISE: return 0.85f;
        case CompositePrimitive::SLOW_RISE:  return 0.75f;
        case CompositePrimitive::QUICK_FALL: return 0.85f;
        case CompositePrimitive::LIGHT_TICK:       return 0.55f;
        case CompositePrimitive::LOW_TICK:   return 0.45f;
        default:                             return 0.0f;  // NOOP
    }
}

int32_t Vibrator::primitiveDurationMs(CompositePrimitive p) {
    switch (p) {
        case CompositePrimitive::CLICK:      return 12;
        case CompositePrimitive::THUD:       return 40;
        case CompositePrimitive::SPIN:       return 65;
        case CompositePrimitive::QUICK_RISE: return 50;
        case CompositePrimitive::SLOW_RISE:  return 90;
        case CompositePrimitive::QUICK_FALL: return 50;
        case CompositePrimitive::LIGHT_TICK:       return 12;
        case CompositePrimitive::LOW_TICK:   return 12;
        default:                             return 0;  // NOOP
    }
}

// ---- IVibrator core -------------------------------------------------------

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeoutMs <= 0) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    cancelSequence();
    std::lock_guard<std::mutex> lock(mMutex);
    if (mFd < 0) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    stopLocked();
    if (!playConstantLocked(timeoutMs))
        return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
    armCallbackLocked(timeoutMs, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    cancelSequence();
    std::lock_guard<std::mutex> lock(mMutex);
    stopLocked();
    cancelCallbackLocked();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    if (amplitude <= 0.0f || amplitude > 1.0f)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    std::lock_guard<std::mutex> lock(mMutex);
    mAmplitude = amplitude;
    if (mIsOn && mFd >= 0 && mEffectId >= 0) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 mDeadline - std::chrono::steady_clock::now())
                                 .count();
        if (remaining <= 0) {
            stopLocked();
            return ndk::ScopedAStatus::ok();
        }
        struct ff_effect effect;
        memset(&effect, 0, sizeof(effect));
        effect.type = FF_CONSTANT;
        effect.id = mEffectId;
        effect.u.constant.level = scaleMagnitude(mAmplitude);
        effect.replay.length = static_cast<uint16_t>(std::min<long>(remaining, 0xffff));
        if (ioctl(mFd, EVIOCSFF, &effect) >= 0) {
            struct input_event play;
            memset(&play, 0, sizeof(play));
            play.type = EV_FF;
            play.code = mEffectId;
            play.value = 1;
            (void)write(mFd, &play, sizeof(play));
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    float strengthScale;
    switch (strength) {
        case EffectStrength::LIGHT:  strengthScale = 0.6f; break;
        case EffectStrength::MEDIUM: strengthScale = 0.8f; break;
        case EffectStrength::STRONG: strengthScale = 1.0f; break;
        default: return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    std::vector<Step> steps;
    switch (effect) {
        case Effect::CLICK:
            steps.push_back({0, strengthScale, kClickDurationMs});
            break;
        case Effect::TICK:
        case Effect::TEXTURE_TICK:
            steps.push_back({0, strengthScale * 0.7f, kTickDurationMs});
            break;
        case Effect::HEAVY_CLICK:
            steps.push_back({0, strengthScale, kHeavyClickDurationMs});
            break;
        case Effect::DOUBLE_CLICK:
            steps.push_back({0, strengthScale, kClickDurationMs});
            steps.push_back({kDoubleClickGapMs, strengthScale, kClickDurationMs});
            break;
        default:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    if (mFd < 0) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);

    int32_t total = 0;
    for (const auto& s : steps) total += s.delayMs + std::max(s.durationMs, kMinDurationMs);

    startSequence(std::move(steps), callback);
    *_aidl_return = total;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::TICK, Effect::TEXTURE_TICK,
                     Effect::DOUBLE_CLICK, Effect::HEAVY_CLICK};
    return ndk::ScopedAStatus::ok();
}

void Vibrator::triggerHapticHint(int32_t durationMs, float amplitude) {
    cancelSequence();
    std::lock_guard<std::mutex> lock(mMutex);
    if (mFd < 0) return;
    stopLocked();
    mAmplitude = (amplitude > 0.0f && amplitude <= 1.0f) ? amplitude : 0.6f;
    playConstantLocked(durationMs);  // kernel auto-stops after replay.length
}

// ---- composition / primitives ---------------------------------------------

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = 1000;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = 256;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    *supported = {
            CompositePrimitive::NOOP,       CompositePrimitive::CLICK,
            CompositePrimitive::THUD,       CompositePrimitive::SPIN,
            CompositePrimitive::QUICK_RISE, CompositePrimitive::SLOW_RISE,
            CompositePrimitive::QUICK_FALL, CompositePrimitive::LIGHT_TICK,
            CompositePrimitive::LOW_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
    *durationMs = primitiveDurationMs(primitive);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.size() > 256)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    if (mFd < 0) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);

    std::vector<Step> steps;
    steps.reserve(composite.size());
    for (const auto& ce : composite) {
        if (ce.delayMs < 0 || ce.delayMs > 1000)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        if (ce.scale < 0.0f || ce.scale > 1.0f)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

        int32_t dur = primitiveDurationMs(ce.primitive);
        if (ce.primitive == CompositePrimitive::NOOP || dur == 0) {
            // NOOP: honor only the delay.
            steps.push_back({ce.delayMs, kMinPulseAmplitude, 0});
            continue;
        }
        float amp = primitiveAmplitude(ce.primitive) * (ce.scale > 0.0f ? ce.scale : 1.0f);
        steps.push_back({ce.delayMs, amp, dur});
    }

    startSequence(std::move(steps), callback);
    return ndk::ScopedAStatus::ok();
}

// ---- unsupported surface --------------------------------------------------

ndk::ScopedAStatus Vibrator::setExternalControl(bool /*enabled*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t, Effect, EffectStrength) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz) {
    *resonantFreqHz = 175.0f;  // qcom-hv-haptics typical resonant frequency ~175 Hz
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    *qFactor = 15.0f;  // Typical Q factor for LRA
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* freqResolutionHz) {
    *freqResolutionHz = 1.0f;  // 1 Hz resolution
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* freqMinimumHz) {
    *freqMinimumHz = 100.0f;  // Minimum frequency for qcom-hv-haptics LRA
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) {
    // Simple linear map: frequency range 100-300 Hz maps to 0.3-1.0 amplitude
    *_aidl_return = {100.0f, 0.3f, 300.0f, 1.0f};
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* supported) {
    supported->clear();
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>&,
                                         const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::vibrator
