// =============================================================================
// Engine/StructOffsetDiag.h
// -----------------------------------------------------------------------------
// "Offsets" panel — drives StructOffsetFinder and shows the result table.
// Lets the operator (a) re-run the finder against a chosen sample size,
// (b) inspect per-offset score breakdowns, (c) push the discovered offsets
// into the global Off:: table.
// =============================================================================

#pragma once

#define INTERNAL __attribute__((visibility("hidden")))

namespace StructOffsetDiag {

INTERNAL void RenderContent() noexcept;

}  // namespace StructOffsetDiag
