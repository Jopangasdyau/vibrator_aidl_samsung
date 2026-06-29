#!/system/bin/sh
# Run on device (adb shell, root recommended): sh /data/local/tmp/probe.sh
# Confirms which input device is the haptics LRA and whether it speaks FF.

echo "== input devices =="
for d in /dev/input/event*; do
    name=$(cat /sys/class/input/$(basename "$d")/device/name 2>/dev/null)
    echo "$d -> ${name:-?}"
done

echo
echo "== /proc/bus/input/devices (look for Handlers=...eventN and 'qcom-hv-haptics') =="
cat /proc/bus/input/devices 2>/dev/null | grep -iE "Name=|Handlers=|haptic|vibrat"

echo
echo "== current vibrator HAL services =="
getprop | grep -i vibrator
echo "--- registered binder services ---"
( service list 2>/dev/null || /system/bin/service list 2>/dev/null ) | grep -i vibrator

echo
echo "== node perms (must be group-readable by 'input') =="
ls -lZ /dev/input/event* 2>/dev/null

echo
echo "== recent vibrator/SELinux denials =="
dmesg 2>/dev/null | grep -iE "avc:.*vibrator|qcom-hv-haptics" | tail -20
logcat -d -b all 2>/dev/null | grep -iE "vibrator-ffinput|avc:.*hal_vibrator|VibratorManagerService" | tail -30
