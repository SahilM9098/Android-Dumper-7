// =============================================================================
// Engine/ObjectArrayDiag.h
// -----------------------------------------------------------------------------
// Step-3 sanity panel for UE::ObjectArray. Lets the operator paste a raw
// libUE4 offset for GUObjectArray, run Init, and inspect:
//   * detected layout (chunked / flat) + item stride
//   * Num()
//   * first N objects (index, address, internal index)
//   * full-walk count via ForEach (skips null & faults)
//
// The OffsetFinder will replace the manual hex input later, but until then
// this is how we exercise the array layer in-game.
// =============================================================================

#pragma once

#define INTERNAL __attribute__((visibility("hidden")))

namespace ObjectArrayDiag {
    // Tab-body renderer — caller owns the ImGui window.
    INTERNAL void RenderContent() noexcept;
}
