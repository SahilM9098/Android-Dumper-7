// =============================================================================
// Dumper/DumperUI.cpp
// =============================================================================

#include "DumperUI.h"
#include "SDKDumper.h"

#include "imgui.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace DumperUI {

namespace {

    std::string GetPackageName() {
        int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return {};
        }

        char buffer[256] = {};
        ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);

        if (count <= 0) {
            return {};
        }

        std::string package(buffer);

        // If running in a secondary process like com.game.name:remote,
        // keep only the base package name.
        size_t colon = package.find(':');
        if (colon != std::string::npos) {
            package.resize(colon);
        }

        return package;
    }

// Persistent state across frames. The default output path is ARK-specific —
// the user can edit before clicking Start.
std::string g_outputDir =
            "/sdcard/Android/media/" + GetPackageName() + "/SDK";

}  // namespace

void RenderContent() noexcept {
    auto p = SDKDumper::Snapshot();
    bool running = SDKDumper::IsRunning();

    ImGui::TextDisabled("SDK Dumper — emits Dumper-7-style headers per package");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(440.0f);
    ImGui::InputText("Output dir", g_outputDir.data(), sizeof(g_outputDir),
                     running ? ImGuiInputTextFlags_ReadOnly : 0);

    if (running) ImGui::BeginDisabled();
    if (ImGui::Button("Start dump")) SDKDumper::Start(g_outputDir);
    if (running) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!running) ImGui::BeginDisabled();
    if (ImGui::Button("Cancel")) SDKDumper::RequestCancel();
    if (!running) ImGui::EndDisabled();

    ImGui::Separator();

    ImGui::Text("phase:    %s", SDKDumper::PhaseName(p.phase));
    ImGui::Text("objects:  %d processed / %d total", p.processedObjects, p.totalObjects);
    ImGui::Text("packages: %d / %d  (current: %s)",
                p.writtenPackages, p.totalPackages,
                p.currentPackage.empty() ? "-" : p.currentPackage.c_str());
    ImGui::Text("counts:   classes=%d  structs=%d  enums=%d  functions=%d",
                p.classCount, p.structCount, p.enumCount, p.functionCount);

    if (p.totalPackages > 0 && p.phase == SDKDumper::Phase::Writing) {
        float pct = (float)p.writtenPackages / (float)p.totalPackages;
        ImGui::ProgressBar(pct, ImVec2(0.0f, 0.0f));
    } else if (p.totalObjects > 0 &&
               (p.phase == SDKDumper::Phase::Collecting ||
                p.phase == SDKDumper::Phase::Sorting)) {
        float pct = (float)p.processedObjects / (float)p.totalObjects;
        ImGui::ProgressBar(pct, ImVec2(0.0f, 0.0f));
    }

    ImGui::Separator();

    if (p.phase == SDKDumper::Phase::Done) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f),
                           "Done in %.2f s. Output: %s",
                           p.elapsedMicros / 1e6, p.outputDir.c_str());
    } else if (p.phase == SDKDumper::Phase::Failed) {
        ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.45f, 1.0f),
                           "Failed: %s", p.error.c_str());
    } else if (p.phase == SDKDumper::Phase::Cancelled) {
        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.40f, 1.0f),
                           "Cancelled.");
    } else if (p.phase == SDKDumper::Phase::Idle) {
        ImGui::TextDisabled(
                "Run all offset finders (Offsets tab → Apply each) before dumping.");
    }
}

}  // namespace DumperUI
