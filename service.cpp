#define LOG_TAG "vibrator-ffinput"

#include "Vibrator.h"
#include "SehExtension.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::vibrator::Vibrator;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    auto vib = ndk::SharedRefBase::make<Vibrator>();
    const std::string instance = std::string(Vibrator::descriptor) + "/default";

    // Attach Samsung's ISehVibrator extension so OneUI's libvibratorservice
    // (getSehHal) finds it: prevents the bootloop AND renders SemHaptic /
    // haptic-engine effects by routing perform*() to the LRA. Harmless on
    // non-Samsung systems (nobody queries the extension).
    ndk::SpAIBinder binder = vib->asBinder();
    std::weak_ptr<Vibrator> weakVib = vib;
    vibratorffinput::attachSamsungSehExtension(
            binder.get(), [weakVib](int32_t durationMs, float amplitude) {
                if (auto v = weakVib.lock()) v->triggerHapticHint(durationMs, amplitude);
            });

    binder_status_t status = AServiceManager_addService(binder.get(), instance.c_str());
    if (status != STATUS_OK) {
        LOG(FATAL) << "Failed to register " << instance << " (status=" << status << ")";
        return EXIT_FAILURE;
    }
    LOG(INFO) << "vibrator-ffinput service ready: " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should never return
}
