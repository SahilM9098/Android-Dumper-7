#!/bin/bash
# Scripted SDK dump over adb (rooted phone): start the game, inject the dumper,
# let it dump without the overlay, pull the SDK.
#
#   INJECTOR=<arm64 ptrace injector> tools/dump.sh <libmenu.so> <local out dir> [seconds to main menu] [package]
#
# The injector is run as `injector <pid> <payload.so> <package>`. The payload is
# staged in the game's own lib/arm64 folder (restorecon), injected, then removed.
# `setprop debug.dumper7.autostart 3` makes the dumper start the whole pipeline
# 3 s after it loads (MainPanel::AutoStartIfRequested); the property is reset
# at the end. Progress: `adb logcat -s Dumper7`.
set -euo pipefail

LIB=${1:?usage: dump.sh <libmenu.so> <out dir> [load wait] [package]}
OUT=${2:?usage: dump.sh <libmenu.so> <out dir> [load wait] [package]}
LOAD_WAIT=${3:-75}
PKG=${4:-com.studiowildcard.arkuse}
INJECTOR=${INJECTOR:?set INJECTOR to the arm64 injector binary}
REMOTE_SDK=/sdcard/Android/media/$PKG/SDK

adb push "$LIB" /data/local/tmp/dumper7-libmenu.so >/dev/null
adb push "$INJECTOR" /data/local/tmp/dumper7-injector >/dev/null
adb shell "su -c 'chmod 755 /data/local/tmp/dumper7-injector; rm -rf $REMOTE_SDK'"
adb shell setprop debug.dumper7.autostart 3
adb logcat -c

adb shell am force-stop "$PKG"
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null
echo "game started; waiting ${LOAD_WAIT}s for the main menu"
sleep "$LOAD_WAIT"

PID=$(adb shell pidof "$PKG" | tr -d '\r')
[ -n "$PID" ] || { echo "game is not running"; exit 1; }
APK=$(adb shell pm path "$PKG" | tr -d '\r' | grep '/base.apk$' | sed 's/^package://')
STAGED="${APK%/base.apk}/lib/arm64/libmenu_$(date +%s).so"
echo "pid=$PID staged=$STAGED"
adb shell "su -c 'cp /data/local/tmp/dumper7-libmenu.so $STAGED && chmod 755 $STAGED && restorecon $STAGED && /data/local/tmp/dumper7-injector $PID $STAGED $PKG 2>&1; rm -f $STAGED'" | tail -n 15

echo "waiting for the dump to finish"
for _ in $(seq 1 180); do
    if adb logcat -d -s Dumper7 | grep -q '\[sdk\] Finished phase='; then break; fi
    sleep 5
done
adb logcat -d -s Dumper7 > "$OUT.logcat.txt"
grep -E '\[sdk\] Finished|\[sdk\] Collected|\[pipeline\]|classflags|funclayout|containers\]|\[sdk\]\[basic\]' "$OUT.logcat.txt" | tail -n 20

rm -rf "$OUT"
mkdir -p "$OUT"
# One archive instead of thousands of files over adb.
adb shell "su -c 'tar -C /sdcard/Android/media/$PKG -czf /data/local/tmp/dumper7-sdk.tgz SDK && chmod 644 /data/local/tmp/dumper7-sdk.tgz'"
adb pull /data/local/tmp/dumper7-sdk.tgz "$OUT.tgz" >/dev/null
tar -C "$OUT" -xzf "$OUT.tgz"
adb shell "su -c 'rm -f /data/local/tmp/dumper7-sdk.tgz'"
adb shell setprop debug.dumper7.autostart 0
adb shell am force-stop "$PKG"
echo "pulled to $OUT/SDK"
