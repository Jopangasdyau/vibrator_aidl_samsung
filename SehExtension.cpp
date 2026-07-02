#define LOG_TAG "vibrator-ffinput"

#include "SehExtension.h"

#include <aidl/vendor/samsung/hardware/vibrator/BnSehVibrator.h>
#include <android-base/logging.h>
#include <android/binder_auto_utils.h>
#include <android/binder_ibinder.h>

#include <algorithm>
#include <cstdint>

namespace vibratorffinput {
namespace {

namespace seh = aidl::vendor::samsung::hardware::vibrator;
using ndk::ScopedAStatus;

// Default pulse used when a packet's intended duration/intensity can't be derived.
constexpr int32_t kDefaultDurationMs = 10;
constexpr float kDefaultAmplitude = 0.70f;

// Map a raw SehHapticEnginePacket (4 int32 fields, exact semantics unknown) to a
// felt pulse. Heuristic + logged so the mapping can be tuned from real values.
struct Pulse {
    int32_t durationMs;
    float amplitude;
};

float normalizeIntensity(int32_t v) {
    if (v <= 0) return kDefaultAmplitude;
    float a;
    if (v <= 100) a = v / 100.0f;          // 0..100 scale
    else if (v <= 10000) a = v / 10000.0f; // 0..10000 scale
    else a = kDefaultAmplitude;
    return std::clamp(a, 0.30f, 1.0f);
}

int32_t clampDuration(int32_t v) {
    if (v < 5 || v > 5000) return kDefaultDurationMs;
    return std::clamp(v, 10, 300);
}

class SehVibrator : public seh::BnSehVibrator {
  public:
    explicit SehVibrator(std::function<void(int32_t, float)> play) : mPlay(std::move(play)) {}

    // ---- capabilities ----
    ScopedAStatus supportsHapticEngine(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsEnhancedSamsungHapticPattern(bool* r) override { *r = true; return ok(); }
    ScopedAStatus supportsAmplitudeControl(bool* r) override { *r = true; return ok(); }
    ScopedAStatus supportsForceTouchAmplitudeControl(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsFrequencyControl(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsPrebakedHapticPattern(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsFolderStatusSetting(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsFifoHapticEngine(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsHybridHapticEngine(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsEnhancedHybridHapticPattern(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsFolderTentModeStatusSetting(bool* r) override { *r = false; return ok(); }
    ScopedAStatus supportsHasFeature(bool* r) override { *r = false; return ok(); }
    ScopedAStatus hasFeature(const std::string&, bool* r) override { *r = false; return ok(); }
    ScopedAStatus getNumberOfPrebakedHapticPattern(int32_t* r) override { *r = 0; return ok(); }
    ScopedAStatus getAmplitudeList(seh::SehAmplitudeType, std::vector<int32_t>* r) override {
        r->clear();
        return ok();
    }

    // ---- setters (accepted, no-op) ----
    ScopedAStatus setAmplitude(int32_t a) override {
        mAmplitude = a;
        return ok();
    }
    ScopedAStatus setForceTouchAmplitude(int32_t) override { return ok(); }
    ScopedAStatus setFrequencyType(seh::SehFrequencyType) override { return ok(); }
    ScopedAStatus setFolderStatus(seh::SehFolderStatus) override { return ok(); }
    ScopedAStatus setFolderTentModeStatus(seh::SehFolderTentModeStatus) override { return ok(); }

    // ---- playback -> drive the LRA ----
    ScopedAStatus performHapticEngine(const std::vector<seh::SehHapticEnginePacket>& pkts,
                                      int32_t loop) override {
        if (!pkts.empty()) {
            const auto& p = pkts[0];
            LOG(INFO) << "Seh HE n=" << pkts.size() << " loop=" << loop << " f=[" << p.field0
                      << "," << p.field1 << "," << p.field2 << "," << p.field3 << "]";
            Pulse pulse = derive(p);
            play(pulse);
        } else {
            play({kDefaultDurationMs, kDefaultAmplitude});
        }
        return ok();
    }

    ScopedAStatus performCommonInputff(const std::vector<seh::SehCommonInputffPacket>& pkts, bool,
                                       int32_t) override {
        if (!pkts.empty()) {
            const auto& p = pkts[0];
            LOG(INFO) << "Seh CI n=" << pkts.size() << " f=[" << p.field0 << "," << p.field1 << ","
                      << p.field2 << "," << p.field3 << "," << p.field4 << "]";
            play({clampDuration(p.field1), normalizeIntensity(p.field2)});
        } else {
            play({kDefaultDurationMs, kDefaultAmplitude});
        }
        return ok();
    }

    ScopedAStatus performHybridHapticEngine(const std::vector<seh::SehHybridHapticEnginePacket>& p,
                                            int32_t) override {
        play({kDefaultDurationMs, kDefaultAmplitude});
        (void)p;
        return ok();
    }
    ScopedAStatus performPlaybackEngine(const std::vector<seh::SehPlaybackPacket>& p, bool,
                                        int32_t) override {
        play({kDefaultDurationMs, kDefaultAmplitude});
        (void)p;
        return ok();
    }
    ScopedAStatus performPrebakedHapticPattern(int32_t, int32_t intensity, bool) override {
        play({kDefaultDurationMs, normalizeIntensity(intensity)});
        return ok();
    }
    ScopedAStatus performFifoHapticEngine(seh::SehFifoType) override {
        play({kDefaultDurationMs, kDefaultAmplitude});
        return ok();
    }

    // perform(Effect, strength, callback) -> duration. Strength: 0=light,1=medium,2=strong.
    ScopedAStatus perform(::aidl::android::hardware::vibrator::Effect, int32_t strength,
                          const std::shared_ptr<::aidl::android::hardware::vibrator::IVibratorCallback>&,
                          int32_t* _aidl_return) override {
        float amp = strength <= 0 ? 0.5f : (strength == 1 ? 0.75f : 1.0f);
        Pulse pulse{kDefaultDurationMs, amp};
        play(pulse);
        *_aidl_return = pulse.durationMs;
        return ok();
    }

  private:
    static ScopedAStatus ok() { return ScopedAStatus::ok(); }

    Pulse derive(const seh::SehHapticEnginePacket& p) {
        // Best-effort: field1 ~ duration(ms), field3 ~ intensity. Clamped + safe
        // fallback so it always produces a felt pulse; refine once field values
        // from logcat are known.
        int32_t dur = clampDuration(p.field1);
        float amp = normalizeIntensity(p.field3);
        if (mAmplitude > 0 && mAmplitude <= 10000) {
            float scaled = mAmplitude <= 100 ? mAmplitude / 100.0f : mAmplitude / 10000.0f;
            amp = std::clamp(amp * std::clamp(scaled, 0.3f, 1.0f), 0.3f, 1.0f);
        }
        return {dur, amp};
    }

    void play(const Pulse& p) {
        if (mPlay) mPlay(p.durationMs, p.amplitude);
    }

    std::function<void(int32_t, float)> mPlay;
    int32_t mAmplitude = 0;
};

std::shared_ptr<SehVibrator> gSeh;  // kept alive for the process lifetime

}  // namespace

bool attachSamsungSehExtension(AIBinder* serviceBinder,
                               std::function<void(int32_t, float)> playHint) {
    if (serviceBinder == nullptr) return false;
    gSeh = ndk::SharedRefBase::make<SehVibrator>(std::move(playHint));
    ndk::SpAIBinder sehBinder = gSeh->asBinder();
    binder_status_t st = AIBinder_setExtension(serviceBinder, sehBinder.get());
    if (st != STATUS_OK) {
        LOG(ERROR) << "Seh: AIBinder_setExtension failed (" << st << ")";
        return false;
    }
    LOG(INFO) << "Seh: ISehVibrator V5 attached (real BnSehVibrator)";
    return true;
}

}  // namespace vibratorffinput
