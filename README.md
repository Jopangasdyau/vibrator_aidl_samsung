# vibrator-ffinput — AIDL IVibrator HAL for qcom-hv-haptics (input force-feedback)

A standalone AIDL `android.hardware.vibrator` service that drives a qcom-hv-haptics
LRA exposed as a Linux **force-feedback input device** (`/dev/input/eventN`,
name `qcom-hv-haptics`). Built for an Android 16 / OneUI 8.5 port on Poco F5.

It auto-detects the haptics node (no hard dependency on `event6`) and implements:

| IVibrator method      | Behavior                                                       |
|-----------------------|----------------------------------------------------------------|
| `getCapabilities`     | `ON_CALLBACK | PERFORM_CALLBACK | AMPLITUDE_CONTROL`           |
| `on` / `off`          | `FF_CONSTANT` upload + play/stop (this is what OneUI uses most) |
| `setAmplitude`        | scales `FF_CONSTANT` level, live-updates a playing effect       |
| `perform`             | CLICK/TICK/DOUBLE_CLICK/HEAVY_CLICK emulated as short pulses     |
| compose / PWLE / etc. | report unsupported → framework falls back cleanly               |

Interface is pinned to **frozen V2** so the same source compiles on Android 13–16.

---

## 0. Probe the device first (recommended)

```
adb push probe.sh /data/local/tmp/ && adb shell sh /data/local/tmp/probe.sh
```

Confirm there is an `eventN` whose name contains `haptic`/`qcom-hv-haptics`. If the
auto-detect ever picks the wrong node, force it:

```
adb shell setprop persist.vendor.vibrator.ffinput.device /dev/input/event6
```

(make it permanent in your vendor `build.prop` or an init `.rc`).

---

## 1. Compile (you need this once — produces an arm64 binary)

You said you port at the image level, so you have no compiler locally. Pick one:

### Option A — inside any AOSP/LineageOS tree (most reliable)
```
mkdir -p <tree>/vendor/ffinput/vibrator
cp Vibrator.* service.cpp Android.bp *.rc *.xml <tree>/vendor/ffinput/vibrator/
cd <tree> && source build/envsetup.sh && lunch <any arm64 target>
mm -j$(nproc) android.hardware.vibrator-service.ffinput
# output: out/target/product/*/vendor/bin/hw/android.hardware.vibrator-service.ffinput
```
Any arm64 `lunch` target works — you only need the binary, not a full ROM.

### Option B — standalone NDK (no AOSP tree)
Requires the `android.hardware.vibrator-V2-ndk` static/shared lib + generated
headers. If you don't have them, Option A is far less painful. Ask and I'll
generate a CMake/`ndk-build` setup that vendors the AIDL stubs.

---

## 2. Install into your port (image-level)

Mount/unpack `vendor` (and `system_ext`/`product` if that's where your manifest
lives) and place:

```
vendor/bin/hw/android.hardware.vibrator-service.ffinput            (the compiled binary, 0755 root:shell)
vendor/etc/init/android.hardware.vibrator-service.ffinput.rc       (the .rc)
vendor/etc/vintf/manifest/android.hardware.vibrator-service.ffinput.xml   (the .xml fragment)
```

Then:

1. **Remove/disable the old vibrator HAL** so two don't fight for `IVibrator/default`:
   delete its binary + `.rc`, and drop its `<hal>` entry from the vendor VINTF
   manifest. (If you keep a Xiaomi `IVibratorExt` service, that's fine — different
   interface; only the standard `android.hardware.vibrator` must be unique.)

2. **SELinux** — apply `sepolicy/`:
   - file label: add the line in `sepolicy/file_contexts` to your
     `vendor/etc/selinux/vendor_file_contexts`.
   - rules: append `sepolicy/image-level.cil` to your `vendor_sepolicy.cil`
     **or** `sepolicy/image-level.rule` if you inject via Magisk.
   - In a source tree instead, use `sepolicy/hal_vibrator_ffinput.te` + `file_contexts`.

3. **Node permissions** — ensure ueventd gives the `input` group access (default is
   `/dev/input/event*  0660  root  input`, which the `group input` in the `.rc`
   already covers). If your kernel differs, add to `ueventd.rc`:
   ```
   /dev/input/event*   0660   root   input
   ```

4. Set context on the pushed files if editing a live image:
   ```
   chcon u:object_r:hal_vibrator_default_exec:s0 \
       vendor/bin/hw/android.hardware.vibrator-service.ffinput
   ```

Repack, flash `vendor` (+ whichever partition holds the manifest), reboot.

---

## 3. Verify

```
adb shell getprop | grep vibrator-ffinput            # service should be running
adb logcat -d | grep -i vibrator-ffinput             # "service ready" + "Using haptics FF device: ..."
adb shell cmd vibrator_manager synced -d 'on 500'    # 0.5s buzz (Android 13+)
# or:
adb shell cmd vibrator vibrate 500                   # legacy form on some builds
```

If silent, re-run `probe.sh` and check for `avc: denied ... hal_vibrator_default
... input_device` (→ sepolicy not applied) or the wrong `eventN` (→ set the
`persist.vendor.vibrator.ffinput.device` prop).
```
