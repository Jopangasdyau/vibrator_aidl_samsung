package vendor.samsung.hardware.vibrator;

import vendor.samsung.hardware.vibrator.SehHapticEnginePacket;
import vendor.samsung.hardware.vibrator.SehCommonInputffPacket;
import vendor.samsung.hardware.vibrator.SehPlaybackPacket;
import vendor.samsung.hardware.vibrator.SehHybridHapticEnginePacket;
import vendor.samsung.hardware.vibrator.SehFrequencyType;
import vendor.samsung.hardware.vibrator.SehFolderStatus;
import vendor.samsung.hardware.vibrator.SehAmplitudeType;
import vendor.samsung.hardware.vibrator.SehFifoType;
import vendor.samsung.hardware.vibrator.SehFolderTentModeStatus;
import android.hardware.vibrator.Effect;
import android.hardware.vibrator.IVibratorCallback;

// Reconstructed from vendor.samsung.hardware.vibrator-V5-ndk.so (method order ==
// transaction codes 1..27, verified against the BpSehVibrator proxies). Field
// counts of the packets verified via readFromParcel; names are placeholders
// (wire format depends only on count/type/order, all int32).
@VintfStability
interface ISehVibrator {
    boolean supportsAmplitudeControl();                                   // 1
    boolean supportsForceTouchAmplitudeControl();                         // 2
    boolean supportsFrequencyControl();                                   // 3
    boolean supportsHapticEngine();                                       // 4
    boolean supportsPrebakedHapticPattern();                             // 5
    boolean supportsFolderStatusSetting();                               // 6
    boolean supportsEnhancedSamsungHapticPattern();                      // 7
    void setAmplitude(int amplitude);                                    // 8
    void setForceTouchAmplitude(int amplitude);                         // 9
    void setFrequencyType(SehFrequencyType type);                       // 10
    void setFolderStatus(SehFolderStatus status);                       // 11
    int getNumberOfPrebakedHapticPattern();                             // 12
    void performHapticEngine(in SehHapticEnginePacket[] packets, int loop);          // 13
    void performPrebakedHapticPattern(int patternId, int intensity, boolean aligned); // 14
    int[] getAmplitudeList(SehAmplitudeType type);                      // 15
    int perform(Effect effect, int strength, IVibratorCallback callback);            // 16
    void performPlaybackEngine(in SehPlaybackPacket[] packets, boolean aligned, int loop); // 17
    boolean supportsFifoHapticEngine();                                 // 18
    boolean supportsHybridHapticEngine();                               // 19
    boolean supportsEnhancedHybridHapticPattern();                     // 20
    void performFifoHapticEngine(SehFifoType type);                    // 21
    void performHybridHapticEngine(in SehHybridHapticEnginePacket[] packets, int loop); // 22
    boolean supportsFolderTentModeStatusSetting();                     // 23
    void setFolderTentModeStatus(SehFolderTentModeStatus status);      // 24
    boolean supportsHasFeature();                                       // 25
    boolean hasFeature(String feature);                                 // 26
    void performCommonInputff(in SehCommonInputffPacket[] packets, boolean aligned, int loop); // 27
}
