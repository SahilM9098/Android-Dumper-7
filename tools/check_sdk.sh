#!/bin/bash
# Compile-checks a dumped SDK with the NDK's clang: SDK.hpp and every
# Private/*_functions.cpp, syntax only. The generated headers carry
# static_asserts for every struct/class member offset and size, so 0 errors
# means the C++ layout equals the one the engine reported.
#
#   tools/check_sdk.sh <dump>/SDK [jobs]
#
# NDK: $ANDROID_NDK_HOME, else the newest one under ~/Android/Sdk/ndk.
set -uo pipefail

SDK=${1:?usage: check_sdk.sh <dump>/SDK [jobs]}
JOBS=${2:-3}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Android/Sdk/ndk/* 2>/dev/null | sort -V | tail -n 1)}
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++
[ -x "$CLANG" ] || { echo "no NDK clang at $CLANG"; exit 2; }
[ -f "$SDK/SDK.hpp" ] || { echo "no SDK.hpp in $SDK"; exit 2; }

FLAGS=(--target=aarch64-linux-android26 -std=c++17 -fsyntax-only -ferror-limit=0)
LOG=$(mktemp -d)
trap 'rm -rf "$LOG"' EXIT

echo "SDK.hpp ..."
"$CLANG" "${FLAGS[@]}" "$SDK/SDK.hpp" > "$LOG/SDK.hpp.log" 2>&1

count=$(ls "$SDK"/Private/*.cpp 2>/dev/null | wc -l)
echo "Private: $count files, $JOBS at a time ..."
export CLANG LOG
ls "$SDK"/Private/*.cpp 2>/dev/null | xargs -P "$JOBS" -I{} sh -c \
    '"$CLANG" '"${FLAGS[*]}"' "{}" > "$LOG/$(basename "{}").log" 2>&1'

errors=$(cat "$LOG"/*.log | grep -c 'error:')
warnings=$(cat "$LOG"/*.log | grep -c 'warning:')
echo "errors=$errors warnings=$warnings"
if [ "$errors" -gt 0 ] || [ "$warnings" -gt 0 ]; then
    cat "$LOG"/*.log | grep -E 'error:|warning:' | sed -E 's/^[^:]+:[0-9]+:[0-9]+: //' \
        | sort | uniq -c | sort -rn | head -20
    echo "first errors:"
    cat "$LOG"/*.log | grep -A2 'error:' | head -30
fi
[ "$errors" -eq 0 ]
