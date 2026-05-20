// =============================================================================
// Engine/ObjectArrayDiag.cpp
// -----------------------------------------------------------------------------
// ImGui-only test harness for ObjectArray. State is held in a file-local
// struct so re-running Init() is cheap; the panel is safe to leave open
// across frames.
// =============================================================================

#include "ObjectArrayDiag.h"

#include "ObjectArray.h"
#include "OffsetFinder.h"
#include "SafeMemory.h"
#include "UEStructs.h"
#include "Utility.h"
#include "imgui.h"

#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace ObjectArrayDiag {

namespace {

constexpr int kHexBufSize = 32;
constexpr int kPreviewCount = 16;

struct Preview {
    int32_t   index           = 0;
    uintptr_t objectAddr      = 0;
    int32_t   internalIndex   = 0;
    bool      ok              = false;
};

struct PanelState {
    char                 hexBuf[kHexBufSize] = "0x";
    char                 patternBuf[256]    = "";
    bool                 lastInitOk         = false;
    std::string          status             = "Pick a method below, or paste an offset / pattern as a fallback.";
    std::vector<Preview> preview;
    int32_t              walkLiveCount      = -1;
    int32_t              walkFaultCount     = -1;
    std::string          autoFindMethod;
    uint32_t             autoFindCandidates = 0;
    uint64_t             autoFindMicros     = 0;
    std::vector<std::string> autoFindTrace;

    // Loose-scan diagnostic state.
    std::vector<OffsetFinder::LooseCandidate> looseHits;
    bool                                       looseAllLibs = true;
    int                                        looseLimit    = 32;
};

INTERNAL PanelState& State() noexcept {
    static PanelState s;
    return s;
}

INTERNAL uintptr_t ParseHex(const char* s) noexcept {
    if (s == nullptr) return 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uintptr_t v = 0;
    while (*s) {
        char c = *s++;
        uintptr_t d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

INTERNAL void RefreshPreview(PanelState& ps) noexcept {
    ps.preview.clear();
    if (!UE::ObjectArray::IsInitialized()) return;

    const int32_t n = UE::ObjectArray::Num();
    const int32_t lim = (n < kPreviewCount) ? n : kPreviewCount;

    SafeMemory::ScopedSigSegvGuard guard;
    for (int32_t i = 0; i < lim; ++i) {
        Preview p;
        p.index = i;
        guard.Try([&] {
            UE::UObject* obj = UE::ObjectArray::GetByIndex(i);
            if (obj == nullptr) return;
            p.objectAddr = reinterpret_cast<uintptr_t>(obj);
            auto idx = SafeMemory::SafeReadAny<int32_t>(
                    p.objectAddr + Off::UObject::InternalIndex);
            if (idx) {
                p.internalIndex = *idx;
                p.ok = true;
            }
        });
        ps.preview.push_back(p);
    }
}

INTERNAL void DoInit(PanelState& ps, uintptr_t libBase) noexcept {
    ps.preview.clear();
    ps.walkLiveCount = -1;
    ps.walkFaultCount = -1;

    uintptr_t off  = ParseHex(ps.hexBuf);
    if (off == 0) {
        ps.status = "offset parse failed (use 0xHEX)";
        ps.lastInitOk = false;
        return;
    }
    if (libBase == 0) {
        ps.status = "SafeMemory not initialized — run SafeMemory diag first";
        ps.lastInitOk = false;
        return;
    }

    uintptr_t addr = libBase + off;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Init: libBase=0x%lx + off=0x%lx -> addr=0x%lx",
                  (unsigned long)libBase, (unsigned long)off, (unsigned long)addr);
    DLOGI("[OAD] %s", buf);

    ps.lastInitOk = UE::ObjectArray::Init(addr);
    if (ps.lastInitOk) {
        std::snprintf(buf, sizeof(buf),
                      "OK — %s, stride=0x%x, num=%d, inner=0x%lx",
                      UE::ObjectArray::IsChunked() ? "chunked" : "flat",
                      UE::ObjectArray::ItemSize(),
                      UE::ObjectArray::Num(),
                      (unsigned long)UE::ObjectArray::InnerAddress());
        ps.status = buf;
        RefreshPreview(ps);
    } else {
        ps.status = "Init failed — wrong offset, or layout not recognized at this address";
    }
}

enum class FindKind { Symbol, StringRef, CodeRef, Pattern, BssScan, DeepScan, Chain };

INTERNAL OffsetFinder::Result RunFinder(FindKind k, const UE4Info& info,
                                        const char* patternSig) noexcept {
    switch (k) {
        case FindKind::Symbol:    return OffsetFinder::FindGObjects_Symbol   (info);
        case FindKind::StringRef: return OffsetFinder::FindGObjects_StringRef(info);
        case FindKind::CodeRef:   return OffsetFinder::FindGObjects_CodeRef  (info);
        case FindKind::Pattern:   return OffsetFinder::FindGObjects_Pattern  (info, patternSig);
        case FindKind::BssScan:   return OffsetFinder::FindGObjects_BssScan  (info);
        case FindKind::DeepScan:  return OffsetFinder::FindGObjects_DeepScan (info);
        case FindKind::Chain:     return OffsetFinder::FindGObjects          (info);
    }
    return {};
}

INTERNAL const char* FindKindLabel(FindKind k) noexcept {
    switch (k) {
        case FindKind::Symbol:    return "symbol-only";
        case FindKind::StringRef: return "string-ref-only";
        case FindKind::CodeRef:   return "code-ref-only";
        case FindKind::Pattern:   return "pattern-only";
        case FindKind::BssScan:   return "bss-scan-only";
        case FindKind::DeepScan:  return "deep-struct-only";
        case FindKind::Chain:     return "chain (sym -> string -> code -> bss -> deep)";
    }
    return "?";
}

INTERNAL void DoFind(PanelState& ps, FindKind kind) noexcept {
    ps.preview.clear();
    ps.walkLiveCount = -1;
    ps.walkFaultCount = -1;

    UE4Info info = FindAndOpenLibUE4();
    if (info.base == 0) {
        ps.status = "libUE4/libUnreal not located in this process";
        ps.lastInitOk = false;
        return;
    }

    OffsetFinder::Result fr = RunFinder(kind, info, ps.patternBuf);
    ps.autoFindMethod     = OffsetFinder::MethodName(fr.method);
    ps.autoFindCandidates = fr.candidatesScanned;
    ps.autoFindMicros     = fr.elapsedMicros;
    ps.autoFindTrace      = fr.traceLines;

    if (!fr.ok) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "[%s] failed: %s (%.1f ms)",
                      FindKindLabel(kind), fr.detail.c_str(), fr.elapsedMicros / 1000.0);
        ps.status = buf;
        ps.lastInitOk = false;
        return;
    }

    std::snprintf(ps.hexBuf, kHexBufSize, "0x%lx", static_cast<unsigned long>(fr.offset));

    ps.lastInitOk = UE::ObjectArray::Init(fr.address);
    if (ps.lastInitOk) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "[%s] hit via %s — offset 0x%lx, %s, stride=0x%x, num=%d (%.1f ms)",
                      FindKindLabel(kind),
                      ps.autoFindMethod.c_str(),
                      static_cast<unsigned long>(fr.offset),
                      UE::ObjectArray::IsChunked() ? "chunked" : "flat",
                      UE::ObjectArray::ItemSize(),
                      UE::ObjectArray::Num(),
                      fr.elapsedMicros / 1000.0);
        ps.status = buf;
        RefreshPreview(ps);
    } else {
        ps.status = "Finder produced a candidate but final Init failed (unexpected)";
    }
}

INTERNAL void DoLooseScan(PanelState& ps) noexcept {
    UE4Info info = FindAndOpenLibUE4();
    ps.looseHits = OffsetFinder::FindGObjects_Loose(
            info, ps.looseAllLibs, static_cast<size_t>(ps.looseLimit));

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Loose scan: %zu candidate%s (%s)",
                  ps.looseHits.size(),
                  ps.looseHits.size() == 1 ? "" : "s",
                  ps.looseAllLibs ? "all libraries" : "libUE4 only");
    ps.status = buf;
}

INTERNAL void DoFullWalk(PanelState& ps) noexcept {
    if (!UE::ObjectArray::IsInitialized()) {
        ps.status = "Init first";
        return;
    }
    int32_t live = 0;
    SafeMemory::ScopedSigSegvGuard::ResetFaultCount();
    UE::ObjectArray::ForEach([&](UE::UObject* /*obj*/) {
        ++live;
    });
    ps.walkLiveCount  = live;
    ps.walkFaultCount = SafeMemory::ScopedSigSegvGuard::FaultCount();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "ForEach: %d live objects walked, %d faults skipped",
                  live, ps.walkFaultCount);
    ps.status = buf;
    DLOGI("[OAD] %s", buf);
}

}  // namespace

void RenderContent() noexcept {
    PanelState& ps = State();

    const uintptr_t libBase = SafeMemory::LibBase();
    ImGui::Text("libUE4 base : 0x%016lx", (unsigned long)libBase);
    if (libBase == 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                           "SafeMemory not initialized. Run the SafeMemory panel first.");
    }

    ImGui::Separator();

    // --- Auto-find: one button per individual method, plus the chain ---
    ImGui::TextDisabled("Each method runs in isolation so you can test them individually:");
    if (ImGui::Button("Symbol"))           DoFind(ps, FindKind::Symbol);
    ImGui::SameLine();
    if (ImGui::Button("String-ref"))       DoFind(ps, FindKind::StringRef);
    ImGui::SameLine();
    if (ImGui::Button("Code-ref"))         DoFind(ps, FindKind::CodeRef);
    ImGui::SameLine();
    if (ImGui::Button("BSS scan"))         DoFind(ps, FindKind::BssScan);

    if (ImGui::Button("Auto (chain all)")) DoFind(ps, FindKind::Chain);
    ImGui::SameLine();
    ImGui::TextDisabled("symbol -> string -> code -> bss");

    // Pattern: paste any IDA-style sig. Empty = button disabled.
    ImGui::Separator();
    ImGui::TextDisabled("Byte-pattern (IDA syntax: \"AA BB ?? CC\"):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##patternSig", ps.patternBuf, sizeof(ps.patternBuf));
    if (ImGui::Button("Pattern scan")) DoFind(ps, FindKind::Pattern);
    ImGui::SameLine();
    ImGui::TextDisabled("(matches in .text, then walks ADRP+ADD/LDR forward to validate)");

    if (!ps.autoFindMethod.empty()) {
        ImGui::Text("last find: method=%s  candidates=%u  elapsed=%.1f ms",
                    ps.autoFindMethod.c_str(),
                    ps.autoFindCandidates,
                    ps.autoFindMicros / 1000.0);
    }

    if (!ps.autoFindTrace.empty()) {
        if (ImGui::CollapsingHeader("Probe trace (per-attempt detail)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("probeTrace", ImVec2(0, 200), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& line : ps.autoFindTrace) {
                ImGui::TextUnformatted(line.c_str());
            }
            ImGui::EndChild();
        }
    }

    ImGui::Separator();

    // --- Loose scan diagnostic (when every Find* method fails) ---
    ImGui::TextDisabled("Loose scan (no Probe validation — diagnostic for cold targets):");
    ImGui::Checkbox("Walk all libraries", &ps.looseAllLibs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderInt("limit", &ps.looseLimit, 4, 128);
    ImGui::SameLine();
    if (ImGui::Button("Loose scan")) DoLooseScan(ps);

    if (!ps.looseHits.empty()) {
        if (ImGui::BeginTable("loose", 7,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
              | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0, 220))) {
            ImGui::TableSetupColumn("addr",     ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableSetupColumn("num",      ImGuiTableColumnFlags_WidthFixed,    70.0f);
            ImGui::TableSetupColumn("Objects",  ImGuiTableColumnFlags_WidthStretch, 0.20f);
            ImGui::TableSetupColumn("idx[0]",   ImGuiTableColumnFlags_WidthFixed,    60.0f);
            ImGui::TableSetupColumn("vt-X",     ImGuiTableColumnFlags_WidthFixed,    50.0f);
            ImGui::TableSetupColumn("lib",      ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableSetupColumn("pick",     ImGuiTableColumnFlags_WidthFixed,    60.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < ps.looseHits.size(); ++i) {
                const auto& c = ps.looseHits[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("0x%016lx", (unsigned long)c.address);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", c.numElements);
                ImGui::TableSetColumnIndex(2); ImGui::Text("0x%012lx", (unsigned long)c.objectsPtr);
                ImGui::TableSetColumnIndex(3);
                if (c.firstObjIdxReadable) ImGui::Text("%d", c.firstObjIdx);
                else                       ImGui::TextDisabled("?");
                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(c.vtableExec
                        ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
                        : ImVec4(0.85f, 0.45f, 0.45f, 1.0f),
                        c.vtableExec ? "yes" : "no");
                ImGui::TableSetColumnIndex(5);
                ImGui::TextWrapped("%s", c.libraryName.c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("use")) {
                    std::snprintf(ps.hexBuf, kHexBufSize, "0x%lx",
                                  (libBase != 0 && c.address >= libBase)
                                          ? (unsigned long)(c.address - libBase)
                                          : (unsigned long)c.address);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // First object hex dump for the top candidate — fastest way to spot
        // a layout drift (look for the InternalIndex byte position, vtable
        // pointer alignment, FName field, etc.)
        const auto& top = ps.looseHits[0];
        if (top.objHexReadable) {
            ImGui::TextDisabled("Top candidate's first object @ 0x%lx (first 64 bytes):",
                                (unsigned long)top.firstObject);
            char hexLine[160];
            for (int row = 0; row < 4; ++row) {
                int p = 0;
                p += std::snprintf(hexLine + p, sizeof(hexLine) - p, "%02x:", row * 16);
                for (int col = 0; col < 16; ++col) {
                    p += std::snprintf(hexLine + p, sizeof(hexLine) - p, " %02x",
                                       top.objHex[row * 16 + col]);
                }
                ImGui::TextUnformatted(hexLine);
            }
        }
    }

    ImGui::Separator();

    // --- Manual offset (last-resort backup) ---
    ImGui::TextDisabled("Manual override (backup if every auto method fails):");
    ImGui::Text("GUObjectArray offset (rel. libUE4 base):");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##gobjOffset", ps.hexBuf, kHexBufSize,
                     ImGuiInputTextFlags_CharsHexadecimal);

    ImGui::SameLine();
    if (ImGui::Button("Init")) {
        DoInit(ps, libBase);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        UE::ObjectArray::Reset();
        ps = PanelState{};
    }

    ImGui::Separator();
    ImVec4 statColor = ps.lastInitOk
            ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
            : ImVec4(0.85f, 0.75f, 0.40f, 1.0f);
    ImGui::TextColored(statColor, "%s", ps.status.c_str());

    if (UE::ObjectArray::IsInitialized()) {
        ImGui::Separator();
        ImGui::Text("layout    : %s", UE::ObjectArray::IsChunked() ? "chunked" : "flat");
        ImGui::Text("itemSize  : 0x%x", UE::ObjectArray::ItemSize());
        ImGui::Text("Num()     : %d", UE::ObjectArray::Num());
        ImGui::Text("inner @   : 0x%016lx", (unsigned long)UE::ObjectArray::InnerAddress());
        ImGui::Text("source @  : 0x%016lx", (unsigned long)UE::ObjectArray::SourceAddress());

        ImGui::Separator();
        if (ImGui::Button("Refresh first 16")) RefreshPreview(ps);
        ImGui::SameLine();
        if (ImGui::Button("Walk all (count live)")) DoFullWalk(ps);

        if (ps.walkLiveCount >= 0) {
            ImGui::Text("Walk: live=%d  faults=%d", ps.walkLiveCount, ps.walkFaultCount);
        }

        if (ImGui::BeginTable("preview", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("i",        ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("UObject*", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("internal", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("ok",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableHeadersRow();
            for (const auto& p : ps.preview) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.index);
                ImGui::TableSetColumnIndex(1); ImGui::Text("0x%016lx", (unsigned long)p.objectAddr);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%d", p.internalIndex);
                ImGui::TableSetColumnIndex(3);
                ImVec4 col = p.ok ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
                                  : ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
                ImGui::TextColored(col, "%s", p.ok ? "ok" : "—");
            }
            ImGui::EndTable();
        }
    }
}

}  // namespace ObjectArrayDiag
