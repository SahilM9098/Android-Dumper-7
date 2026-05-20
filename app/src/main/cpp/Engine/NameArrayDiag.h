// =============================================================================
// Engine/NameArrayDiag.h
// -----------------------------------------------------------------------------
// NameArray (FNamePool) tab. Mirrors the ObjectArray tab in spirit:
// auto-find via Symbol, manual offset fallback, and a per-attempt probe
// trace so failures are diagnostic rather than opaque. Once Init succeeds,
// shows the detected BlocksOffset, the first N names, and a "lookup by
// index" tester so the operator can validate against known indices.
// =============================================================================

#pragma once

#define INTERNAL __attribute__((visibility("hidden")))

namespace NameArrayDiag {
    INTERNAL void RenderContent() noexcept;
}
