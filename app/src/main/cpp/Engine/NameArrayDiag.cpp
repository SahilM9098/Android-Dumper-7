// =============================================================================
// Engine/NameArrayDiag.cpp
// -----------------------------------------------------------------------------
// Step-1 NameArray panel: Symbol button, manual offset, name preview, lookup
// tester. Other finder methods (string-ref, code-ref, BSS, pattern) land
// once Symbol is verified working — same incremental approach as ObjectArray.
// =============================================================================

#include "NameArrayDiag.h"

#include "NameArray.h"
#include "OffsetFinder.h"
#include "SafeMemory.h"
#include "UEStructs.h"
#include "Utility.h"
#include "imgui.h"

#include <android/log.h>
#include <cstdio>
#include <string>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace NameArrayDiag {

namespace {

constexpr int kHexBufSize    = 32;
constexpr int kPreviewCount  = 16;

struct PanelState {
    char hexBuf[kHexBufSize]  = "0x";
    char lookupBuf[16]        = "0";
    char patternBuf[256]      = "";

    bool        lastInitOk    = false;
    std::string status        = "Pick a method below or paste a NamePoolData offset.";

    std::vector<std::pair<int32_t, std::string>> preview;
    std::vector<std::string> trace;

    std::string lookupResult;
    std::string lastFindMethod;
    uint32_t    lastFindCandidates = 0;
    uint64_t    elapsedMicros = 0;
};

INTERNAL PanelState& State() noexcept {
    static PanelState s;
    return s;
}

INTERNAL uintptr_t ParseHex(const char* s) noexcept {
    if (!s) return 0;
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

INTERNAL int32_t ParseDecimal(const char* s) noexcept {
    if (!s) return 0;
    int32_t v = 0;
    while (*s == ' ' || *s == '\t') ++s;
    bool neg = false;
    if (*s == '-') { neg = true; ++s; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return neg ? -v : v;
}

INTERNAL void RefreshPreview(PanelState& ps) noexcept {
    ps.preview.clear();
    if (!UE::NameArray::IsInitialized()) return;
    // Walk block 0 sequentially. Iterating raw indices 0..N doesn't work —
    // FName indices are (block << bits) | (byteOffset / Stride), so only the
    // values that land on an entry header decode correctly.
    ps.preview = UE::NameArray::WalkBlock(/*blockIndex*/ 0, kPreviewCount);
}

enum class FindKind { Symbol, FuncWalk, StringRef, CodeRef, Pattern, BssScan, Chain };

INTERNAL OffsetFinder::Result RunFinder(FindKind k, const UE4Info& info,
                                        const char* patternSig) noexcept {
    switch (k) {
        case FindKind::Symbol:    return OffsetFinder::FindGNames_Symbol   (info);
        case FindKind::FuncWalk:  return OffsetFinder::FindGNames_FuncWalk (info);
        case FindKind::StringRef: return OffsetFinder::FindGNames_StringRef(info);
        case FindKind::CodeRef:   return OffsetFinder::FindGNames_CodeRef  (info);
        case FindKind::Pattern:   return OffsetFinder::FindGNames_Pattern  (info, patternSig);
        case FindKind::BssScan:   return OffsetFinder::FindGNames_BssScan  (info);
        case FindKind::Chain:     return OffsetFinder::FindGNames          (info);
    }
    return {};
}

INTERNAL const char* FindKindLabel(FindKind k) noexcept {
    switch (k) {
        case FindKind::Symbol:    return "symbol-only";
        case FindKind::FuncWalk:  return "func-walk-only";
        case FindKind::StringRef: return "string-ref-only";
        case FindKind::CodeRef:   return "code-ref-only";
        case FindKind::Pattern:   return "pattern-only";
        case FindKind::BssScan:   return "bss-scan-only";
        case FindKind::Chain:     return "chain (sym -> func -> string -> code -> bss)";
    }
    return "?";
}

INTERNAL void DoFind(PanelState& ps, FindKind kind) noexcept {
    ps.preview.clear();
    ps.lookupResult.clear();

    UE4Info info = FindAndOpenLibUE4();
    if (info.base == 0) {
        ps.status = "libUE4/libUnreal not located in this process";
        ps.lastInitOk = false;
        return;
    }

    OffsetFinder::Result fr = RunFinder(kind, info, ps.patternBuf);
    ps.trace              = fr.traceLines;
    ps.elapsedMicros      = fr.elapsedMicros;
    ps.lastFindMethod     = OffsetFinder::MethodName(fr.method);
    ps.lastFindCandidates = fr.candidatesScanned;

    if (!fr.ok) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[%s] failed: %s (%.1f ms)",
                      FindKindLabel(kind), fr.detail.c_str(),
                      fr.elapsedMicros / 1000.0);
        ps.status = buf;
        ps.lastInitOk = false;
        return;
    }

    std::snprintf(ps.hexBuf, kHexBufSize, "0x%lx",
                  static_cast<unsigned long>(fr.offset));

    ps.lastInitOk = UE::NameArray::Init(fr.address);
    if (ps.lastInitOk) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "[%s] hit via %s — offset 0x%lx, BlocksOffset=0x%x (%.1f ms)",
                      FindKindLabel(kind),
                      ps.lastFindMethod.c_str(),
                      (unsigned long)fr.offset,
                      UE::NameArray::BlocksOffset(),
                      fr.elapsedMicros / 1000.0);
        ps.status = buf;
        RefreshPreview(ps);
    } else {
        ps.status = "Finder produced a candidate but final Init failed (unexpected)";
    }
}

INTERNAL void DoManual(PanelState& ps, uintptr_t libBase) noexcept {
    ps.preview.clear();
    ps.lookupResult.clear();
    ps.trace.clear();

    uintptr_t off = ParseHex(ps.hexBuf);
    if (off == 0) {
        ps.status = "offset parse failed (use 0xHEX)";
        ps.lastInitOk = false;
        return;
    }
    if (libBase == 0) {
        ps.status = "SafeMemory not initialized — open SafeMemory tab first";
        ps.lastInitOk = false;
        return;
    }

    uintptr_t addr = libBase + off;
    ps.lastInitOk = UE::NameArray::Init(addr);
    char buf[200];
    if (ps.lastInitOk) {
        std::snprintf(buf, sizeof(buf),
                      "Manual OK — addr=0x%lx, BlocksOffset=0x%x",
                      (unsigned long)addr, UE::NameArray::BlocksOffset());
        ps.status = buf;
        RefreshPreview(ps);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Manual Init failed at 0x%lx — wrong offset or unrecognized layout",
                      (unsigned long)addr);
        ps.status = buf;
    }
}

INTERNAL void DoLookup(PanelState& ps) noexcept {
    if (!UE::NameArray::IsInitialized()) {
        ps.lookupResult = "(init NameArray first)";
        return;
    }
    int32_t idx = ParseDecimal(ps.lookupBuf);
    SafeMemory::ScopedSigSegvGuard guard;
    std::string n;
    guard.Try([&] { n = UE::NameArray::GetName(idx); });
    if (n.empty()) {
        ps.lookupResult = "(empty / out-of-range)";
    } else {
        ps.lookupResult = n;
    }
}

}  // namespace

void RenderContent() noexcept {
    PanelState& ps = State();
    const uintptr_t libBase = SafeMemory::LibBase();

    ImGui::Text("libUE4 base : 0x%016lx", (unsigned long)libBase);
    if (libBase == 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                           "Open the SafeMemory tab first.");
    }

    ImGui::Separator();

    // --- Auto-find: each method runs in isolation, plus the chain ---
    ImGui::TextDisabled("Each method runs in isolation so you can test them individually:");
    if (ImGui::Button("Symbol"))     DoFind(ps, FindKind::Symbol);
    ImGui::SameLine();
    if (ImGui::Button("Func-walk"))  DoFind(ps, FindKind::FuncWalk);
    ImGui::SameLine();
    if (ImGui::Button("String-ref")) DoFind(ps, FindKind::StringRef);
    ImGui::SameLine();
    if (ImGui::Button("Code-ref"))   DoFind(ps, FindKind::CodeRef);
    ImGui::SameLine();
    if (ImGui::Button("BSS scan"))   DoFind(ps, FindKind::BssScan);

    if (ImGui::Button("Auto (chain all)")) DoFind(ps, FindKind::Chain);
    ImGui::SameLine();
    ImGui::TextDisabled("symbol -> func -> string -> code -> bss");

    ImGui::Separator();
    ImGui::TextDisabled("Byte-pattern (IDA syntax: \"AA BB ?? CC\"):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##namePatternSig", ps.patternBuf, sizeof(ps.patternBuf));
    if (ImGui::Button("Pattern scan")) DoFind(ps, FindKind::Pattern);
    ImGui::SameLine();
    ImGui::TextDisabled("(matches in .text, then walks ADRP+ADD/LDR forward to validate)");

    if (!ps.lastFindMethod.empty()) {
        ImGui::Text("last find: method=%s  candidates=%u  elapsed=%.1f ms",
                    ps.lastFindMethod.c_str(),
                    ps.lastFindCandidates,
                    ps.elapsedMicros / 1000.0);
    }

    ImGui::Separator();

    // --- Manual offset (always available fallback) ---
    ImGui::TextDisabled("Manual override (rel. libUE4 base):");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##namesOffset", ps.hexBuf, kHexBufSize,
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::Button("Init")) DoManual(ps, libBase);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        UE::NameArray::Reset();
        ps = PanelState{};
    }

    ImGui::Separator();
    ImVec4 statColor = ps.lastInitOk
            ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
            : ImVec4(0.85f, 0.75f, 0.40f, 1.0f);
    ImGui::TextColored(statColor, "%s", ps.status.c_str());

    if (!ps.trace.empty()) {
        if (ImGui::CollapsingHeader("Probe trace (per-attempt detail)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("nameProbeTrace", ImVec2(0, 180), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& line : ps.trace) {
                ImGui::TextUnformatted(line.c_str());
            }
            ImGui::EndChild();
        }
    }

    if (UE::NameArray::IsInitialized()) {
        ImGui::Separator();
        ImGui::Text("source @     : 0x%016lx", (unsigned long)UE::NameArray::SourceAddress());
        ImGui::Text("blocksArray @: 0x%016lx", (unsigned long)UE::NameArray::BlocksArrayAddress());
        ImGui::Text("BlocksOffset : 0x%x",     UE::NameArray::BlocksOffset());
        ImGui::Text("indirection  : %s",
                    UE::NameArray::IsDoubleIndirect()
                        ? "double (*source -> &Blocks)"
                        : "direct (source + BlocksOffset)");
        ImGui::Text("hdr layout   : %s",
                    UE::NameArray::IsCasePreserving()
                        ? "case-preserving (15-bit Len)"
                        : "standard (10-bit Len + 5-bit hash)");

        ImGui::Separator();
        ImGui::TextDisabled("Lookup by ComparisonIndex:");
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputText("##nameLookup", ps.lookupBuf, sizeof(ps.lookupBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button("Lookup")) DoLookup(ps);
        if (!ps.lookupResult.empty()) {
            ImGui::SameLine();
            ImGui::Text("-> \"%s\"", ps.lookupResult.c_str());
        }

        ImGui::Separator();
        ImGui::TextDisabled("First %d names:", kPreviewCount);
        if (ImGui::BeginTable("namePreview", 2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("idx",  ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& [idx, name] : ps.preview) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", idx);
                ImGui::TableSetColumnIndex(1);
                if (name.empty()) {
                    ImGui::TextDisabled("(empty)");
                } else {
                    ImGui::TextUnformatted(name.c_str());
                }
            }
            ImGui::EndTable();
        }
    }
}

}  // namespace NameArrayDiag
