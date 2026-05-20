// =============================================================================
// Engine/PatternScanner.h
// -----------------------------------------------------------------------------
// Generic IDA-style byte pattern engine. Pattern syntax:
//
//   "AA BB ?? CC ?D"        — bytes are 2 hex digits; "??" matches any byte;
//                              a single "?" inside a byte means any nibble.
//
// Used for finding instruction prologues, vtable signatures, function
// entries, etc. Not coupled to any specific engine version or game — patterns
// are supplied by the caller (UI, OffsetFinder, etc.).
//
// All scans are guarded by the caller (install ScopedSigSegvGuard if you are
// scanning over potentially unmapped pages — though typically you scan only
// libUE4 segments which are entirely mapped).
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#define INTERNAL __attribute__((visibility("hidden")))

namespace PatternScanner {

struct Pattern {
    std::vector<uint8_t> bytes;       // value at each position
    std::vector<uint8_t> mask;        // 0xFF = match all bits, 0x00 = wildcard byte,
                                       // 0xF0/0x0F = match one nibble only
    bool empty() const noexcept { return bytes.empty(); }
    size_t size() const noexcept { return bytes.size(); }
};

INTERNAL Pattern Parse(const char* sig) noexcept;

INTERNAL uintptr_t Scan(uintptr_t start, size_t size, const Pattern& pat) noexcept;

INTERNAL std::vector<uintptr_t> ScanAll(uintptr_t start, size_t size,
                                        const Pattern& pat,
                                        size_t maxHits = 1024) noexcept;

INTERNAL std::string Describe(const Pattern& pat) noexcept;

}  // namespace PatternScanner
