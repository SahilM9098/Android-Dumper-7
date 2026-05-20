// =============================================================================
// Engine/SafeMemoryDiag.h
// -----------------------------------------------------------------------------
// Step-1 sanity panel for SafeMemory. Locates libUE4.so, runs Init, then
// exercises:
//   * SafeRead<uint32_t>(libBase)                   — must succeed, ELF magic.
//   * SafeRead<uint8_t>(libBase - 1)                — must fail bounds.
//   * SafeRead<uint64_t>(libBase | 1)               — must fail alignment.
//   * ScopedSigSegvGuard.Try( fault @ 0x42 )        — must catch.
//   * ScopedSigSegvGuard.Try( libBase deref )       — must succeed.
//
// Run() is idempotent — first call performs the checks, later calls are no-ops.
// RenderImGui() draws the result table and a rolling log into the current
// ImGui frame. Remove this file once OffsetFinder is wired in.
// =============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#define INTERNAL __attribute__((visibility("hidden")))

namespace SafeMemoryDiag {

struct Check {
    std::string label;
    bool        ok = false;
    std::string detail;
};

struct Report {
    bool                     ranOnce  = false;
    bool                     libFound = false;
    uintptr_t                libBase  = 0;
    std::size_t              segments = 0;
    std::size_t              span     = 0;
    std::string              apkPath;
    std::vector<Check>       checks;
    std::vector<std::string> log;
};

INTERNAL const Report& Run() noexcept;        // safe checks only (auto-runs once)
INTERNAL void RunSigSegvTest() noexcept;       // explicit, deliberately faults
INTERNAL void RenderContent() noexcept;        // tab body — no ImGui::Begin/End

}  // namespace SafeMemoryDiag
