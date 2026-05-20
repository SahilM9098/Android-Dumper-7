// =============================================================================
// Core/ObjectArray.h
// -----------------------------------------------------------------------------
// Singleton wrapper over UE4/UE5 GUObjectArray. Supports both:
//
//   * FFixedUObjectArray         — single contiguous FUObjectItem[]     (UE4.13-)
//   * FChunkedFixedUObjectArray  — array of fixed-size chunk pointers   (UE4.14+)
//
// Init(addr) auto-detects:
//   * chunked vs flat layout
//   * FUObjectItem stride (0x18 vs 0x20)
//   * whether `addr` points at FUObjectArray (in libUE4 BSS) or directly at
//     the inner TUObjectArray member.
//
// All structural reads against libUE4 go through SafeMemory::SafeRead. All
// dereferences into game-heap chunks / items go through SafeReadAny under a
// ScopedSigSegvGuard so a corrupt slot can be skipped instead of crashing.
//
// An optional decryption lambda is applied to every raw pointer read out of
// FUObjectItem.Object (and chunk pointer slots), to support games that XOR
// or otherwise mask stored UObject pointers.
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "SafeMemory.h"
#include "UEStructs.h"

#define INTERNAL __attribute__((visibility("hidden")))

namespace UE {

class ObjectArray {
public:
    using DecryptFn = std::function<uint8_t*(void*)>;

    INTERNAL static bool      Init(uintptr_t gObjectsAddress) noexcept;
    INTERNAL static void      Reset() noexcept;
    INTERNAL static bool      IsInitialized() noexcept;

    INTERNAL static void      SetDecryption(DecryptFn fn) noexcept;

    // Non-mutating layout test — used by OffsetFinder to scan candidates.
    // Probe installs its own SIGSEGV guard; ProbeNoGuard assumes the caller
    // has one already (cheaper for tight scan loops).
    INTERNAL static bool      Probe(uintptr_t address) noexcept;
    INTERNAL static bool      ProbeNoGuard(uintptr_t address) noexcept;

    // Diagnostic: same checks as Probe but accumulates a per-step explanation
    // into `report`. Useful when an address resolves but Probe rejects it —
    // tells you exactly which invariant failed.
    INTERNAL static bool      ProbeReport(uintptr_t address,
                                          std::vector<std::string>& report) noexcept;

    INTERNAL static int32_t   Num() noexcept;
    INTERNAL static UObject*  GetByIndex(int32_t index) noexcept;

    INTERNAL static bool      IsChunked() noexcept;
    INTERNAL static int32_t   ItemSize() noexcept;
    INTERNAL static uintptr_t InnerAddress() noexcept;   // resolved TUObjectArray base
    INTERNAL static uintptr_t SourceAddress() noexcept;  // address passed to Init()

    // Walk all live (non-null) objects under SIGSEGV protection. Per-index
    // reads are wrapped — a fault on one slot skips that slot only.
    template <class F>
    INTERNAL static void ForEach(F&& fn) noexcept {
        const int32_t n = Num();
        if (n <= 0) return;
        SafeMemory::ScopedSigSegvGuard guard;
        for (int32_t i = 0; i < n; ++i) {
            UObject* obj = nullptr;
            if (!guard.Try([&] { obj = GetByIndex(i); })) continue;
            if (obj == nullptr) continue;
            if (!guard.Try([&] { fn(obj); })) continue;
        }
    }
};

}  // namespace UE
