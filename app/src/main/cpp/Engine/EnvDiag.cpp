// =============================================================================
// Engine/EnvDiag.cpp
// -----------------------------------------------------------------------------
// Implementation: enumerate dlpi_phdr, classify segments, render a sortable
// table. Cached on first render — Refresh button forces re-enumeration.
// =============================================================================

#include "EnvDiag.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <link.h>
#include <string>
#include <vector>

#define INTERNAL __attribute__((visibility("hidden")))

namespace EnvDiag {

namespace {

struct LibEntry {
    std::string name;
    uintptr_t   base       = 0;
    size_t      lo         = ~size_t{0};
    size_t      hi         = 0;
    int         loadSegs   = 0;
    int         execSegs   = 0;
    int         writeSegs  = 0;
    bool        ueHint     = false;   // name suggests UE-related
};

INTERNAL bool LooksUE(const std::string& n) noexcept {
    if (n.empty()) return false;
    const char* lower = n.c_str();
    auto containsCi = [](const char* haystack, const char* needle) {
        size_t hlen = std::strlen(haystack), nlen = std::strlen(needle);
        if (nlen > hlen) return false;
        for (size_t i = 0; i + nlen <= hlen; ++i) {
            bool match = true;
            for (size_t j = 0; j < nlen; ++j) {
                char a = haystack[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
                if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
                if (a != b) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    };
    return containsCi(lower, "ue4")
        || containsCi(lower, "ue5")
        || containsCi(lower, "unreal")
        || containsCi(lower, "coreuobject")
        || containsCi(lower, "engine.so")
        || containsCi(lower, "gamethread")
        || containsCi(lower, "libak");
}

INTERNAL std::vector<LibEntry> Enumerate() noexcept {
    std::vector<LibEntry> out;

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* vec = static_cast<std::vector<LibEntry>*>(data);
        LibEntry e;
        e.name = info->dlpi_name ? info->dlpi_name : "";
        e.base = info->dlpi_addr;

        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& p = info->dlpi_phdr[i];
            if (p.p_type != PT_LOAD) continue;
            ++e.loadSegs;
            if (p.p_flags & PF_X) ++e.execSegs;
            if (p.p_flags & PF_W) ++e.writeSegs;
            uintptr_t s = info->dlpi_addr + p.p_vaddr;
            uintptr_t t = s + p.p_memsz;
            if (s < e.lo) e.lo = s;
            if (t > e.hi) e.hi = t;
        }
        e.ueHint = LooksUE(e.name);
        vec->push_back(std::move(e));
        return 0;
    }, &out);

    return out;
}

INTERNAL std::vector<LibEntry>& Cache() noexcept {
    static std::vector<LibEntry> s;
    static bool inited = false;
    if (!inited) { s = Enumerate(); inited = true; }
    return s;
}

}  // namespace

void RenderContent() noexcept {
    auto& libs = Cache();

    ImGui::Text("Loaded shared objects: %zu", libs.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        libs = Enumerate();
    }

    int ueCount = 0;
    for (const auto& l : libs) if (l.ueHint) ++ueCount;
    if (ueCount == 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                           "No UE-suspicious library names detected. The engine binary "
                           "may be renamed or anonymously mapped.");
    } else {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f),
                           "%d library name%s look UE-related", ueCount,
                           ueCount == 1 ? "" : "s");
    }

    ImGui::Separator();

    if (ImGui::BeginTable("libs", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
          | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
            ImVec2(0, 360))) {
        ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Base",   ImGuiTableColumnFlags_WidthFixed,   140.0f);
        ImGui::TableSetupColumn("Span",   ImGuiTableColumnFlags_WidthFixed,    90.0f);
        ImGui::TableSetupColumn("Load",   ImGuiTableColumnFlags_WidthFixed,    50.0f);
        ImGui::TableSetupColumn("X",      ImGuiTableColumnFlags_WidthFixed,    35.0f);
        ImGui::TableSetupColumn("W",      ImGuiTableColumnFlags_WidthFixed,    35.0f);
        ImGui::TableHeadersRow();

        for (const auto& l : libs) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (l.ueHint) {
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f),
                                   "%s", l.name.empty() ? "(main / vdso)" : l.name.c_str());
            } else {
                ImGui::TextWrapped("%s", l.name.empty() ? "(main / vdso)" : l.name.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%016lx", static_cast<unsigned long>(l.base));
            ImGui::TableSetColumnIndex(2);
            size_t span = (l.loadSegs > 0) ? (l.hi - l.lo) : 0;
            ImGui::Text("0x%lx", static_cast<unsigned long>(span));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", l.loadSegs);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", l.execSegs);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", l.writeSegs);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Yellow rows are libraries whose names suggest they might host the UE "
        "runtime. If you don't see libUE4.so / libUnreal.so / libCoreUObject.so "
        "and nothing yellow looks plausible, the engine module is hidden — try "
        "Deep struct (it walks every loaded library). If even Deep struct fails, "
        "the global is likely encrypted or behind anti-tamper.");
}

}  // namespace EnvDiag
