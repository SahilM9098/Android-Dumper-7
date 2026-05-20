// =============================================================================
// Dumper/DumperUI.h
// -----------------------------------------------------------------------------
// Renders the "Dumper" tab inside MainPanel: output-dir input, Start/Cancel
// buttons, live phase + counts + progress bar polling SDKDumper::Snapshot().
// =============================================================================

#pragma once

#define INTERNAL __attribute__((visibility("hidden")))

namespace DumperUI {

INTERNAL void RenderContent() noexcept;

}  // namespace DumperUI
