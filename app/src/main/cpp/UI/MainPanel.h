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

    // Starts the whole pipeline (library, GObjects, GNames, offsets, dump)
    // without the overlay when the system property debug.dumper7.autostart is
    // set to a number of seconds above 0: the wait before starting, so the
    // engine has finished loading. For scripted dumps:
    //   adb shell setprop debug.dumper7.autostart 5
    // Progress goes to logcat (tags Dumper7 and MainPanel). No effect when the
    // property is unset or 0.
    INTERNAL void AutoStartIfRequested() noexcept;
}
