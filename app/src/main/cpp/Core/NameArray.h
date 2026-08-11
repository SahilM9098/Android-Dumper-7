// =============================================================================
// Core/NameArray.h
// -----------------------------------------------------------------------------
// Singleton wrapper around UE4's global name storage. Supports both the
// FNamePool used by UE4.23+ and the older TNameEntryArray used by UE4.22 and
// earlier (including the original ARK Mobile build).
//
// Init(addr) takes either the global storage address or a global pointer slot.
// It identifies FNamePool by probing its Blocks[] pointer array, or validates
// a legacy TNameEntryArray by resolving entry zero.  Both layouts must decode
// FName index 0 to "None".
//
// GetName(index) decomposes ComparisonIndex into (block, offset_in_block),
// dereferences the appropriate Blocks slot, and decodes the FNameEntryHeader
// (2 bytes: bit 0 = wide, bits 1..15 = length) followed by the character
// data. Returns "" on any failure — never throws.
//
// All chunk dereferences go through SafeReadAny under a ScopedSigSegvGuard
// to survive corrupted pool state without crashing the host process.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SafeMemory.h"
#include "UEStructs.h"

#define INTERNAL __attribute__((visibility("hidden")))

namespace UE {

class NameArray {
public:
    INTERNAL static bool        Init(uintptr_t poolAddress) noexcept;
    INTERNAL static void        Reset() noexcept;
    INTERNAL static bool        IsInitialized() noexcept;

    // Non-mutating layout test — used by OffsetFinder to scan candidates.
    INTERNAL static bool        Probe(uintptr_t address) noexcept;
    INTERNAL static bool        ProbeNoGuard(uintptr_t address) noexcept;

    // Diagnostic: same checks as Probe but accumulates per-step explanation.
    INTERNAL static bool        ProbeReport(uintptr_t address,
                                            std::vector<std::string>& report) noexcept;

    // Resolve a name by ComparisonIndex. Returns "" on any failure.
    INTERNAL static std::string GetName(int32_t comparisonIndex) noexcept;

    // Walk consecutive entries inside `blockIndex`, producing up to `maxCount`
    // (comparisonIndex, decoded-name) pairs. Use this for previews — naïvely
    // iterating indices 0..N doesn't work because FName indices are byte-
    // offsets/Stride, so non-zero indices that don't land on an entry header
    // decode to garbage.
    INTERNAL static std::vector<std::pair<int32_t, std::string>>
        WalkBlock(int32_t blockIndex, int32_t maxCount) noexcept;

    // Diag accessors.
    INTERNAL static int32_t     BlocksOffset() noexcept;
    INTERNAL static uintptr_t   SourceAddress() noexcept;
    INTERNAL static uintptr_t   BlocksArrayAddress() noexcept;
    INTERNAL static bool        IsDoubleIndirect() noexcept;
    INTERNAL static bool        IsCasePreserving() noexcept;
    INTERNAL static bool        IsLegacy() noexcept;
};

}  // namespace UE
