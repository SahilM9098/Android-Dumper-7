// =============================================================================
// UI/MainPanel.h
// -----------------------------------------------------------------------------
// One ImGui window for the whole dumper. Each subsystem renders into its own
// tab via a *RenderContent* function. Add new tabs here as more subsystems
// (NameArray, PatternScanner, SDKDumper, etc.) come online.
// =============================================================================

#pragma once

#define INTERNAL __attribute__((visibility("hidden")))

namespace MainPanel {
    INTERNAL void Render() noexcept;
}
