/*
 * AIDL IVibrator HAL for qcom-hv-haptics via the Linux input force-feedback API.
 *
 * Drives an LRA exposed as a force-feedback input device (e.g. /dev/input/eventN,
 * name "qcom-hv-haptics"). Implements:
 *   - on/off + amplitude control (FF_CONSTANT)            -> waveforms, ringtone
 *   - perform() predefined effects                         -> click/tick/...
 *   - compose() composition primitives                     -> Gboard, soft taps,
 *                                                             modern app haptics
 * Short/low pulses are floored to a perceptible duration/amplitude so light
 * keyboard ticks actually move the LRA.
 */
#pragma once

#include <aidl/android/hardware/vibrator/BnVibrator.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::vibrator {

class Vibrator : public BnVibrator {
  public:
    Vibrator();
    ~Vibrator();

    // --- core ---
    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;

    // Fire a single self-stopping pulse. Used by the Samsung Seh extension to
    // render SemHaptic / haptic-engine effects (called from a binder thread).
    void triggerHapticHint(int32_t durationMs, float amplitude);

    // --- composition / primitives (Gboard & modern haptics) ---
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* maxDelayMs) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* supported) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* durationMs) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;

    // --- advertised as unsupported (framework falls back gracefully) ---
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* resonantFreqHz) override;
    ndk::ScopedAStatus getQFactor(float* qFactor) override;
    ndk::ScopedAStatus getFrequencyResolution(float* freqResolutionHz) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* freqMinimumHz) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* durationMs) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* supported) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;

  private:
    // One element of a played sequence: wait delayMs, then buzz at amplitude for durationMs.
    struct Step {
        int32_t delayMs;
        float amplitude;   // (0,1]
        int32_t durationMs;
    };

    bool openDevice();
    bool playConstantLocked(int32_t durationMs);   // requires mMutex
    void stopLocked();                             // requires mMutex
    static int16_t scaleMagnitude(float amplitude);

    // on()-style one-shot completion timer.
    void armCallbackLocked(int32_t durationMs,
                           const std::shared_ptr<IVibratorCallback>& callback);
    void cancelCallbackLocked();

    // perform()/compose() sequencing engine.
    void startSequence(std::vector<Step> steps,
                       const std::shared_ptr<IVibratorCallback>& callback);
    void runSequence(std::vector<Step> steps, std::shared_ptr<IVibratorCallback> callback);
    void cancelSequence();                 // stop + join worker (do NOT hold mMutex)
    bool interruptibleSleepMs(int32_t ms); // returns true if cancelled
    bool sequenceCancelled();

    static float primitiveAmplitude(CompositePrimitive p);
    static int32_t primitiveDurationMs(CompositePrimitive p);

    std::mutex mMutex;
    int mFd = -1;
    int16_t mEffectId = -1;
    float mAmplitude = 1.0f;
    bool mIsOn = false;
    std::chrono::steady_clock::time_point mDeadline;
    std::string mDevicePath;

    // on() completion timer
    std::thread mCallbackThread;
    std::condition_variable mCallbackCv;
    std::mutex mCallbackMutex;
    bool mCallbackCancel = false;
    bool mCallbackPending = false;

    // perform()/compose() worker
    std::thread mWorker;
    std::mutex mWorkerMutex;
    std::condition_variable mWorkerCv;
    bool mWorkerCancel = false;
};

}  // namespace aidl::android::hardware::vibrator
