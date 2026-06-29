#!/usr/bin/env bash
#
# build_hal.sh — reproducible standalone build of the input-ff AIDL vibrator HAL
# for arm64, using the Android NDK on macOS (no AOSP tree required).
#
# It bootstraps everything it needs and is idempotent — re-running only redoes
# missing pieces:
#   1. resolve the NDK toolchain  (env > cache > mounted dmg > .build/*.dmg)
#   2. fetch the AOSP `aidl` compiler + frozen vibrator AIDL sources (if missing)
#   3. generate the NDK C++ backend for android.hardware.vibrator V2 (if missing)
#   4. create libbase/binder shims + a stub libbinder_ndk.so (if missing)
#   5. compile + strip + stage into dist/
#
# Usage:
#   ./build_hal.sh           # build (bootstrap as needed)
#   ./build_hal.sh clean     # remove generated backend, shims, stublibs, out
#   ./build_hal.sh rebuild   # clean then build
#
# Override the NDK location with:  ANDROID_NDK_HOME=/path/to/ndk ./build_hal.sh
set -euo pipefail

# ----------------------------------------------------------------------------
# config
# ----------------------------------------------------------------------------
SRC_ROOT="$(cd "$(dirname "$0")" && pwd)"
B="$SRC_ROOT/.build"
DIST="$SRC_ROOT/dist"
API=34
NAME='vibrator-aidl-jopvan@1.0-service'
NDK_VER="r27c"
NDK_DMG_URL="https://dl.google.com/android/repository/android-ndk-${NDK_VER}-darwin.dmg"
AIDL_VER=2                       # frozen android.hardware.vibrator version
TC_CACHE="/tmp/ndk_tc_path.txt"

GIT_BASE="https://android.googlesource.com/platform"
AIDL_TOOL_URL="$GIT_BASE/prebuilts/build-tools/+/refs/heads/main/darwin-x86/bin/aidl?format=TEXT"
AIDL_PKG_BASE="$GIT_BASE/hardware/interfaces/+/refs/heads/main/vibrator/aidl/aidl_api/android.hardware.vibrator/${AIDL_VER}/android/hardware/vibrator"
NATIVE_BASE="$GIT_BASE/frameworks/native/+/refs/heads/main/libs/binder/ndk"

SRCS=(Vibrator.cpp SehExtension.cpp service.cpp)
AIDL_FILES=(ActivePwle Braking BrakingPwle CompositeEffect CompositePrimitive \
            Effect EffectStrength IVibrator IVibratorCallback IVibratorManager PrimitivePwle)

log(){ printf '\033[1;34m[build]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

# ----------------------------------------------------------------------------
# clean
# ----------------------------------------------------------------------------
if [[ "${1:-}" == "clean" || "${1:-}" == "rebuild" ]]; then
    log "cleaning generated artifacts"
    rm -rf "$B/gen" "$B/gen_seh" "$B/shim" "$B/stublibs" "$B/out"
    [[ "${1:-}" == "clean" ]] && { log "clean done"; exit 0; }
fi

mkdir -p "$B"

# ----------------------------------------------------------------------------
# 1. resolve NDK toolchain
# ----------------------------------------------------------------------------
find_tc_in(){ find "$1" -type d -path '*toolchains/llvm/prebuilt/darwin-x86_64' 2>/dev/null | head -1; }

resolve_toolchain(){
    local tc=""
    # a) explicit env
    if [[ -n "${ANDROID_NDK_HOME:-}" && -d "$ANDROID_NDK_HOME" ]]; then
        tc="$(find_tc_in "$ANDROID_NDK_HOME")"
    fi
    # b) cached path from a previous run
    if [[ -z "$tc" && -f "$TC_CACHE" ]]; then
        local c; c="$(cat "$TC_CACHE" 2>/dev/null || true)"
        [[ -n "$c" && -x "$c/bin/aarch64-linux-android${API}-clang++" ]] && tc="$c"
    fi
    # c) already-mounted dmg or common SDK locations
    if [[ -z "$tc" ]]; then
        tc="$(find_tc_in /tmp/ndk-${NDK_VER} 2>/dev/null || true)"
        [[ -z "$tc" ]] && tc="$(find_tc_in "$HOME/Library/Android/sdk/ndk" 2>/dev/null || true)"
    fi
    # d) mount the dmg we downloaded into .build (survives reboots)
    if [[ -z "$tc" && -f "$B/ndk-${NDK_VER}.dmg" ]]; then
        log "mounting $B/ndk-${NDK_VER}.dmg"
        hdiutil attach "$B/ndk-${NDK_VER}.dmg" -nobrowse -quiet \
            -mountpoint "/tmp/ndk-${NDK_VER}" 2>/dev/null || true
        tc="$(find_tc_in /tmp/ndk-${NDK_VER} 2>/dev/null || true)"
    fi
    # e) last resort: download the dmg, then mount
    if [[ -z "$tc" ]]; then
        log "NDK not found — downloading ${NDK_VER} (~880 MB) into .build/"
        curl -L --fail -o "$B/ndk-${NDK_VER}.dmg" "$NDK_DMG_URL" \
            || die "NDK download failed; set ANDROID_NDK_HOME instead"
        hdiutil attach "$B/ndk-${NDK_VER}.dmg" -nobrowse -quiet \
            -mountpoint "/tmp/ndk-${NDK_VER}" 2>/dev/null || true
        tc="$(find_tc_in /tmp/ndk-${NDK_VER} 2>/dev/null || true)"
    fi
    [[ -n "$tc" ]] || die "could not resolve NDK toolchain; set ANDROID_NDK_HOME=/path/to/ndk"
    echo "$tc" > "$TC_CACHE"
    echo "$tc"
}

TC="$(resolve_toolchain)"
CXX="$TC/bin/aarch64-linux-android${API}-clang++"
CC="$TC/bin/aarch64-linux-android${API}-clang"
NM="$TC/bin/llvm-nm"; STRIP="$TC/bin/llvm-strip"; READELF="$TC/bin/llvm-readelf"
log "toolchain: $TC"

# ----------------------------------------------------------------------------
# 2. AOSP aidl compiler + frozen vibrator AIDL sources
# ----------------------------------------------------------------------------
fetch_b64(){ curl -s --fail --max-time 60 "$1" | base64 -d > "$2"; }

if [[ ! -x "$B/bin/aidl" ]]; then
    log "fetching AOSP aidl compiler"
    mkdir -p "$B/bin"
    fetch_b64 "$AIDL_TOOL_URL" "$B/bin/aidl" || die "aidl download failed"
    chmod +x "$B/bin/aidl"
fi

AIDL_SRC="$B/aidl_src/android/hardware/vibrator"
if [[ ! -f "$AIDL_SRC/IVibrator.aidl" ]]; then
    log "fetching frozen vibrator AIDL v${AIDL_VER} sources"
    mkdir -p "$AIDL_SRC"
    for f in "${AIDL_FILES[@]}"; do
        fetch_b64 "$AIDL_PKG_BASE/$f.aidl?format=TEXT" "$AIDL_SRC/$f.aidl" \
            || die "failed to fetch $f.aidl"
    done
fi

# ----------------------------------------------------------------------------
# 3. generate NDK C++ backend
# ----------------------------------------------------------------------------
if [[ ! -f "$B/gen/include/aidl/android/hardware/vibrator/BnVibrator.h" ]]; then
    log "generating NDK backend"
    rm -rf "$B/gen"; mkdir -p "$B/gen"
    "$B/bin/aidl" --lang=ndk --structured --stability=vintf \
        --version="$AIDL_VER" --min_sdk_version="$API" \
        -I "$B/aidl_src" -o "$B/gen/staging" -h "$B/gen/include" \
        "$AIDL_SRC"/*.aidl || die "aidl codegen failed"
fi

# Samsung Seh extension backend (reconstructed package under seh_aidl/).
if [[ ! -f "$B/gen_seh/include/aidl/vendor/samsung/hardware/vibrator/BnSehVibrator.h" ]]; then
    log "generating Seh NDK backend"
    rm -rf "$B/gen_seh"; mkdir -p "$B/gen_seh"
    "$B/bin/aidl" --lang=ndk --structured --stability=vintf \
        --version=5 --hash=720a16b521507c378f14c516749ae178a60dfc44 --min_sdk_version="$API" \
        -I "$SRC_ROOT/seh_aidl" -I "$B/aidl_src" \
        -o "$B/gen_seh/staging" -h "$B/gen_seh/include" \
        "$SRC_ROOT"/seh_aidl/vendor/samsung/hardware/vibrator/*.aidl || die "seh codegen failed"
fi

# ----------------------------------------------------------------------------
# 4. libbase / binder shims + stub libbinder_ndk.so
# ----------------------------------------------------------------------------
SHIM="$B/shim"
if [[ ! -f "$SHIM/android-base/logging.h" ]]; then
    log "writing libbase/binder shims"
    mkdir -p "$SHIM/android-base" "$SHIM/android"

    cat > "$SHIM/android-base/logging.h" <<'EOF'
#pragma once
#include <android/log.h>
#include <cstdlib>
#include <ostream>
#include <sstream>
#ifndef LOG_TAG
#define LOG_TAG "vibrator-ffinput"
#endif
namespace base_shim {
class LogMessage {
  public:
    LogMessage(int p, bool f) : priority_(p), fatal_(f) {}
    ~LogMessage() {
        __android_log_print(priority_, LOG_TAG, "%s", stream_.str().c_str());
        if (fatal_) std::abort();
    }
    std::ostream& stream() { return stream_; }
  private:
    std::ostringstream stream_; int priority_; bool fatal_;
};
}  // namespace base_shim
#define _LP_VERBOSE ANDROID_LOG_VERBOSE
#define _LP_DEBUG   ANDROID_LOG_DEBUG
#define _LP_INFO    ANDROID_LOG_INFO
#define _LP_WARNING ANDROID_LOG_WARN
#define _LP_ERROR   ANDROID_LOG_ERROR
#define _LP_FATAL   ANDROID_LOG_FATAL
#define _LF_VERBOSE false
#define _LF_DEBUG   false
#define _LF_INFO    false
#define _LF_WARNING false
#define _LF_ERROR   false
#define _LF_FATAL   true
#define LOG(sev)  ::base_shim::LogMessage(_LP_##sev, _LF_##sev).stream()
#define PLOG(sev) LOG(sev)
#define CHECK(c)  if (!(c)) ::base_shim::LogMessage(ANDROID_LOG_FATAL, true).stream() \
                      << "CHECK failed: " #c " "
EOF

    cat > "$SHIM/android-base/properties.h" <<'EOF'
#pragma once
#include <sys/system_properties.h>
#include <string>
namespace android { namespace base {
inline std::string GetProperty(const std::string& key, const std::string& def) {
    char v[PROP_VALUE_MAX] = {0};
    int n = __system_property_get(key.c_str(), v);
    return n > 0 ? std::string(v, (size_t)n) : def;
}
}}  // namespace android::base
EOF

    cat > "$SHIM/android-base/stringprintf.h" <<'EOF'
#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
namespace android { namespace base {
inline std::string StringPrintf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap); va_end(ap);
    std::string out;
    if (n > 0) { std::vector<char> b(n + 1); vsnprintf(b.data(), b.size(), fmt, ap2); out.assign(b.data(), n); }
    va_end(ap2); return out;
}
}}  // namespace android::base
EOF

    # binder_manager.h + binder_process.h are platform-only (absent from the NDK).
    fetch_b64 "$NATIVE_BASE/include_platform/android/binder_manager.h?format=TEXT" \
              "$SHIM/android/binder_manager.h" || die "fetch binder_manager.h failed"
    fetch_b64 "$NATIVE_BASE/include_platform/android/binder_process.h?format=TEXT" \
              "$SHIM/android/binder_process.h" || die "fetch binder_process.h failed"
    # C++ interface utils from AOSP main (matches the aidl codegen ABI).
    fetch_b64 "$NATIVE_BASE/include_cpp/android/binder_interface_utils.h?format=TEXT" \
              "$SHIM/android/binder_interface_utils.h" || die "fetch binder_interface_utils.h failed"
    # one newer C symbol the above header references but r27c headers lack
    cat > "$SHIM/android/binder_ibinder.h" <<'EOF'
#pragma once
#include_next <android/binder_ibinder.h>
#ifdef __cplusplus
extern "C" {
#endif
void AIBinder_Class_setTransactionCodeToFunctionNameMap(AIBinder_Class* clazz,
        const char** map, size_t size) __attribute__((weak));
#ifdef __cplusplus
}
#endif
EOF
fi

STUB="$B/stublibs/libbinder_ndk.so"
if [[ ! -f "$STUB" ]]; then
    log "building stub libbinder_ndk.so"
    mkdir -p "$B/stublibs"
    local_ndk_binder="$TC/sysroot/usr/lib/aarch64-linux-android/${API}/libbinder_ndk.so"
    {
        "$NM" -D --defined-only "$local_ndk_binder" 2>/dev/null \
            | awk '{n=$NF; sub(/@.*/,"",n); print n}' | grep -E '^A(IBinder|Parcel|Status|Binder)'
        # platform-only service/process symbols (not in the NDK stub)
        printf '%s\n' \
            ABinderProcess_setThreadPoolMaxThreadCount ABinderProcess_startThreadPool \
            ABinderProcess_joinThreadPool AServiceManager_addService \
            AServiceManager_addServiceWithFlags AServiceManager_isDeclared \
            AServiceManager_registerLazyService AServiceManager_forceLazyServicesPersist \
            AIBinder_Class_setTransactionCodeToFunctionNameMap
    } | sort -u | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' \
        | awk '{print "void " $1 "(void) {}"}' > "$B/stublibs/stub.c"
    "$CC" --target=aarch64-linux-android${API} -shared -fPIC -fvisibility=default \
        -nostdlib -Wl,-soname,libbinder_ndk.so -o "$STUB" "$B/stublibs/stub.c" \
        || die "stub lib build failed"
fi

# ----------------------------------------------------------------------------
# 5. compile + strip + stage
# ----------------------------------------------------------------------------
log "compiling $NAME"
mkdir -p "$B/out"
SRC_ARGS=(); for s in "${SRCS[@]}"; do SRC_ARGS+=("$SRC_ROOT/$s"); done

"$CXX" \
    -std=gnu++17 -O2 -fPIC -fPIE -pie -Wall \
    -DLOG_TAG='"vibrator-ffinput"' \
    -I"$SHIM" -I"$B/gen/include" -I"$B/gen_seh/include" -I"$SRC_ROOT" \
    "${SRC_ARGS[@]}" \
    "$B"/gen/staging/android/hardware/vibrator/*.cpp \
    "$B"/gen_seh/staging/vendor/samsung/hardware/vibrator/*.cpp \
    -static-libstdc++ \
    -L"$B/stublibs" -lbinder_ndk -llog \
    -o "$B/out/$NAME" || die "compile failed"

log "stripping"
"$STRIP" --strip-all "$B/out/$NAME"

log "staging into dist/"
mkdir -p "$DIST/vendor/bin/hw" "$DIST/vendor/etc/init" "$DIST/vendor/etc/vintf/manifest"
cp "$B/out/$NAME"            "$DIST/vendor/bin/hw/$NAME"
cp "$SRC_ROOT/$NAME.rc"      "$DIST/vendor/etc/init/$NAME.rc"
cp "$SRC_ROOT/$NAME.xml"     "$DIST/vendor/etc/vintf/manifest/$NAME.xml"

echo
log "BUILD OK"
file "$DIST/vendor/bin/hw/$NAME" | sed 's/^/  /'
printf '  sha256 '; shasum -a 256 "$DIST/vendor/bin/hw/$NAME" | awk '{print $1}'
echo "  NEEDED libs:"; "$READELF" -d "$B/out/$NAME" | awk '/NEEDED/{print "   ",$NF}'
echo
echo "Install (vendor partition):"
echo "  /vendor/bin/hw/$NAME                       (0755, chcon u:object_r:hal_vibrator_default_exec:s0)"
echo "  /vendor/etc/init/$NAME.rc"
echo "  /vendor/etc/vintf/manifest/$NAME.xml"
