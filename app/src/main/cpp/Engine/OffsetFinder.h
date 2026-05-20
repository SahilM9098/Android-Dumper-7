// =============================================================================
// Engine/OffsetFinder.h
// -----------------------------------------------------------------------------
// Locates UE4/UE5 reflection roots inside libUE4.so. For each target the
// finder runs the strategies in this order:
//
//   1. Symbol  — try a list of likely mangled / unmangled names via
//                FindSymbolDynamic() and FindSymbolDisk(). Most retail builds
//                strip these, but some leave at least one.
//
//   2. BssScan — walk libUE4's writable PT_LOAD segments at 8-byte stride,
//                calling ObjectArray::Probe() on each candidate. Validation
//                inside Probe is strong enough (chunk-layout sanity +
//                item[0,1].InternalIndex match + UObject vtable in libUE4)
//                that false positives are essentially impossible.
//
//   3. (Manual override stays the user's last resort, handled in the UI.)
//
// This header currently exposes only FindGObjects — NameArray and
// FName::AppendString finders will land alongside it as they come online.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Utility.h"

#define INTERNAL __attribute__((visibility("hidden")))

namespace OffsetFinder {

enum class Method {
    None,
    Symbol,
    StringRef,
    CodeRefQuick, // fast nearby ADRP+ADD/LDR scan; full CodeRef remains fallback
    CodeRef,    // scan every ADRP+ADD/LDR pair in .text, validate writable targets
    Pattern,    // user/known byte pattern matched against .text
    BssScan,    // structural scan, libUE4 writable segments only
    DeepScan,   // structural scan, writable segments across ALL loaded libraries
    Manual,
};

struct Result {
    bool        ok       = false;
    Method      method   = Method::None;
    uintptr_t   address  = 0;   // absolute libUE4 VA where the target was found
    uintptr_t   offset   = 0;   // address - libBase, i.e. the relocatable offset
    std::string detail;         // one-line summary
    std::vector<std::string> traceLines;  // multi-line per-attempt detail (failure diag)
    uint32_t    candidatesScanned = 0;
    uint64_t    elapsedMicros     = 0;
};

struct CodeRefProgress {
    bool     active = false;
    char     targetLabel[32] = {};
    char     stage[16] = {};
    uint32_t segmentIndex = 0;
    uint32_t segmentCount = 0;
    uint64_t totalBytes = 0;
    uint64_t scannedBytes = 0;
    uint64_t segmentBytes = 0;
    uint64_t segmentScannedBytes = 0;
    uint32_t candidatesScanned = 0;
};

// Individual strategies (each callable on its own — used by the diag tab to
// let the operator test methods independently).
INTERNAL Result FindGObjects_Symbol   (const UE4Info& info) noexcept;
INTERNAL Result FindGObjects_StringRef(const UE4Info& info) noexcept;
INTERNAL Result FindGObjects_CodeRefQuick(const UE4Info& info) noexcept;
INTERNAL Result FindGObjects_CodeRef  (const UE4Info& info) noexcept;
INTERNAL Result FindGObjects_Pattern  (const UE4Info& info, const char* patternSig) noexcept;
INTERNAL Result FindGObjects_BssScan  (const UE4Info& info) noexcept;
INTERNAL Result FindGObjects_DeepScan (const UE4Info& info) noexcept;

// =============================================================================
// Diagnostic — does NOT return a Result. Walks libUE4 + every loaded library
// with relaxed thresholds and returns the top N "structurally plausible"
// candidates, with their key field values + a 64-byte hex dump of the first
// item's first object. Strict Probe is replaced with: NumElements > 64,
// MaxElements >= NumElements, Objects pointer non-null + readable.
// Use when every Find* method fails — likely indicates encryption or a
// layout drift, and the dump tells us which.
// =============================================================================

struct LooseCandidate {
    uintptr_t address      = 0;
    bool      chunked      = true;
    int32_t   numElements  = 0;
    int32_t   maxElements  = 0;
    uintptr_t objectsPtr   = 0;
    uintptr_t firstChunk   = 0;
    uintptr_t firstObject  = 0;
    uintptr_t firstVtable  = 0;
    bool      vtableExec   = false;
    int32_t   firstObjIdx  = 0;       // value at first object + 0x0C
    bool      firstObjIdxReadable = false;
    uint8_t   objHex[64]   = {};      // first 64 bytes at firstObject
    bool      objHexReadable = false;
    std::string libraryName;          // host module
};

INTERNAL std::vector<LooseCandidate> FindGObjects_Loose(const UE4Info& info,
                                                        bool allLibraries,
                                                        size_t maxResults) noexcept;

// Convenience chain: Symbol -> StringRef -> CodeRef -> BssScan -> DeepScan.
INTERNAL Result FindGObjects(const UE4Info& info) noexcept;

// =============================================================================
// FNamePool / GNames finders. Same five strategies as GUObjectArray, sharing
// the AArch64 scanning helpers internally (parameterized by a target-specific
// validator). Method enum reused — meaning "found via X strategy" is target-
// independent.
// =============================================================================

INTERNAL Result FindGNames_Symbol   (const UE4Info& info) noexcept;
INTERNAL Result FindGNames_FuncWalk (const UE4Info& info) noexcept;
INTERNAL Result FindGNames_StringRef(const UE4Info& info) noexcept;
INTERNAL Result FindGNames_CodeRefQuick(const UE4Info& info) noexcept;
INTERNAL Result FindGNames_CodeRef  (const UE4Info& info) noexcept;
INTERNAL Result FindGNames_Pattern  (const UE4Info& info, const char* patternSig) noexcept;
INTERNAL Result FindGNames_BssScan  (const UE4Info& info) noexcept;

// Convenience chain: Symbol -> FuncWalk -> StringRef -> CodeRef -> BssScan.
INTERNAL Result FindGNames(const UE4Info& info) noexcept;

INTERNAL CodeRefProgress CodeRefSnapshot() noexcept;
INTERNAL const char* MethodName(Method m) noexcept;

}  // namespace OffsetFinder
