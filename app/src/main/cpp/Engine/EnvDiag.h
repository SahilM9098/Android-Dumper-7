// =============================================================================
// Engine/EnvDiag.h
// -----------------------------------------------------------------------------
// Environment inspection tab. Dumps the live state of the loader: every
// shared object visible via dl_iterate_phdr, with base, span, segment count,
// and a heuristic "looks UE-related" hint (so an unusual library hosting
// the engine doesn't slip past us).
//
// Use this tab when every finder fails — first thing to confirm is that
// the engine module is actually visible under the name we expect.
// =============================================================================

#pragma once

#define INTERNAL __attribute__((visibility("hidden")))

namespace EnvDiag {
    INTERNAL void RenderContent() noexcept;
}
