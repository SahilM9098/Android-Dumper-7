// =============================================================================
// Dumper/SDKDumper.h
// -----------------------------------------------------------------------------
// Background-thread orchestrator that walks GObjects, groups every reflection
// object by its outermost UPackage, topologically sorts inter-package deps,
// and emits Dumper-7-style C++ headers under SDK_DUMP/SDK with an SDK.hpp
// umbrella include at SDK_DUMP/SDK.hpp.
//
// State is shared with the UI via Snapshot() — UI polls, no callbacks.
// Cancellation is cooperative: RequestCancel() flips an atomic; the worker
// checks it at every phase boundary and per-package iteration.
//
// All UE reads go through SafeMemory under ScopedSigSegvGuard, so a corrupt
// slot in GObjects skips one object instead of taking down the host.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

#define INTERNAL __attribute__((visibility("hidden")))

namespace SDKDumper {

enum class Phase {
    Idle,
    Collecting,    // walking GObjects + classifying + grouping by package
    Sorting,       // topo-sorting packages by inter-package SuperStruct deps
    Writing,       // emitting per-package headers
    Done,
    Failed,
    Cancelled,
};

struct Progress {
    Phase phase = Phase::Idle;

    int32_t totalObjects        = 0;
    int32_t processedObjects    = 0;
    int32_t totalPackages       = 0;
    int32_t writtenPackages     = 0;

    int32_t classCount          = 0;
    int32_t structCount         = 0;
    int32_t enumCount           = 0;
    int32_t functionCount       = 0;

    std::string currentPackage;
    std::string outputDir;
    std::string error;
    uint64_t    elapsedMicros   = 0;
};

INTERNAL bool      Start(const std::string& outputDir) noexcept;
INTERNAL void      RequestCancel() noexcept;
INTERNAL Progress  Snapshot() noexcept;
INTERNAL bool      IsRunning() noexcept;

INTERNAL const char* PhaseName(Phase p) noexcept;

// --- SDK generation options (set before calling Start) ---

// When true, function implementations use direct native calls via the
// UFunction::Func pointer instead of ProcessEvent dispatch. Faster at
// runtime but only works for native (C++) functions — Blueprint-only
// functions have Func=nullptr and still use ProcessEvent.
// Default: false (safe ProcessEvent path for all functions).
inline bool g_useDirectCalls = false;

}  // namespace SDKDumper
