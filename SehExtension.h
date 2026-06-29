// Samsung "Seh" vibrator extension.
//
// OneUI's libvibratorservice (AidlHalWrapper) fetches a
// vendor.samsung.hardware.vibrator.ISehVibrator via AIBinder_getExtension() on
// the IVibrator binder. Samsung apps & SystemUI emit SemHaptic effects that are
// ONLY rendered through this extension's perform*() methods -- without it they
// end up IGNORED_UNSUPPORTED (silent).
//
// We attach a binder carrying the real ISehVibrator descriptor and answer its
// transactions directly (codes reverse-engineered from
// vendor.samsung.hardware.vibrator-V5-ndk.so): report the haptic-engine
// capabilities as supported and route every perform*() to a real LRA pulse via
// the supplied callback. No system files are modified.
#pragma once

#include <android/binder_ibinder.h>

#include <functional>

namespace vibratorffinput {

// playHint(durationMs, amplitude) must fire a single self-stopping pulse.
bool attachSamsungSehExtension(AIBinder* serviceBinder,
                               std::function<void(int32_t, float)> playHint);

}  // namespace vibratorffinput
