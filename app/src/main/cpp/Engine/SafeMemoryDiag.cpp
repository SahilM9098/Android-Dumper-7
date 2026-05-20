// =============================================================================
// Engine/SafeMemoryDiag.cpp
// -----------------------------------------------------------------------------
// See SafeMemoryDiag.h for the panel's purpose. Self-contained: depends only
// on Utility.h (FindAndOpenLibUE4), SafeMemory, and ImGui.
// =============================================================================

#include "SafeMemoryDiag.h"

#include "SafeMemory.h"
#include "Utility.h"
#include "imgui.h"

#include <android/log.h>
#include <cstdio>
#include <cstring>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace SafeMemoryDiag {

namespace {

constexpr std::size_t kMaxLog = 200;

INTERNAL Report& State() noexcept {
    static Report s;
    return s;
}

INTERNAL void Log(Report& r, const char* fmt, ...) noexcept {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (r.log.size() >= kMaxLog) {
        r.log.erase(r.log.begin(), r.log.begin() + (r.log.size() - kMaxLog + 1));
    }
    r.log.emplace_back(buf);
    DLOGI("[diag] %s", buf);
}

INTERNAL void Add(Report& r, const char* label, bool ok, const char* detail) noexcept {
    Check c;
    c.label  = label;
    c.ok     = ok;
    c.detail = detail ? detail : "";
    r.checks.push_back(std::move(c));
    Log(r, "%s: %s%s%s", label, ok ? "PASS" : "FAIL",
        detail && *detail ? " — " : "",
        detail ? detail : "");
}

// Dedicated, opaque-to-the-optimizer fault generator. Reading 0x42 is
// guaranteed to fault on Android (page 0 unmapped) without taking the
// compiler down a UB-elimination path.
__attribute__((noinline, optnone))
INTERNAL int DeliberateFault() noexcept {
    volatile int* p = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x42));
    return *p;
}

}  // namespace

const Report& Run() noexcept {
    Report& r = State();
    if (r.ranOnce) return r;
    r.ranOnce = true;

    Log(r, "SafeMemoryDiag::Run start");

    UE4Info info = FindAndOpenLibUE4();
    if (info.base == 0) {
        r.libFound = false;
        Add(r, "FindAndOpenLibUE4", false,
            "libUE4.so / libUnreal.so not loaded in this process");
        Log(r, "Inject into the Unreal game process to populate runtime lib info");
        return r;
    }

    r.libFound = true;
    r.libBase  = info.base;
    r.apkPath  = info.apkPath;

    char baseStr[64];
    std::snprintf(baseStr, sizeof(baseStr), "base=%p path=%s",
                  reinterpret_cast<void*>(info.base), info.apkPath);
    Add(r, "FindAndOpenLibUE4", true, baseStr);

    SafeMemory::Init(info);
    r.segments = 0;  // we expose count through SafeMemory::LibSize indirectly
    r.span     = SafeMemory::LibSize();

    char initStr[96];
    std::snprintf(initStr, sizeof(initStr), "span=0x%zx initialized=%d",
                  r.span, (int)SafeMemory::IsInitialized());
    Add(r, "SafeMemory::Init", SafeMemory::IsInitialized() && r.span > 0, initStr);

    // Read 1: ELF magic at libBase — should be 0x464C457F ('\x7FELF').
    {
        auto v = SafeMemory::SafeRead<uint32_t>(info.base);
        bool ok = v.has_value() && (*v == 0x464C457Fu);
        char d[64];
        if (v) std::snprintf(d, sizeof(d), "got 0x%08x", *v);
        else   std::snprintf(d, sizeof(d), "nullopt");
        Add(r, "SafeRead<u32>(libBase) == ELF magic", ok, d);
    }

    // Read 2: one byte before libBase — must fail bounds.
    {
        auto v = SafeMemory::SafeRead<uint8_t>(info.base - 1);
        bool ok = !v.has_value();
        Add(r, "SafeRead<u8>(libBase-1) rejects out-of-range", ok,
            ok ? "rejected" : "unexpectedly succeeded");
    }

    // Read 3: misaligned u64 — must fail alignment.
    {
        auto v = SafeMemory::SafeRead<uint64_t>(info.base | 1u);
        bool ok = !v.has_value();
        Add(r, "SafeRead<u64> rejects misaligned address", ok,
            ok ? "rejected" : "unexpectedly succeeded");
    }

    Log(r, "SafeMemoryDiag::Run done — %zu safe checks (press button for SIGSEGV guard test)",
        r.checks.size());
    return r;
}

void RunSigSegvTest() noexcept {
    Report& r = State();
    if (!r.libFound) {
        Log(r, "SIGSEGV guard test skipped: Unreal runtime lib not found");
        return;
    }

    // Guard test 1: deliberate fault is caught.
    {
        SafeMemory::ScopedSigSegvGuard guard;
        SafeMemory::ScopedSigSegvGuard::ResetFaultCount();
        bool completed = guard.Try([] { (void)DeliberateFault(); });
        bool caught = !completed && SafeMemory::ScopedSigSegvGuard::FaultCount() > 0;
        Add(r, "ScopedSigSegvGuard catches SIGSEGV at 0x42", caught,
            caught ? "fault intercepted via siglongjmp" : "did not fault as expected");
    }

    // Guard test 2: well-formed work runs to completion.
    {
        SafeMemory::ScopedSigSegvGuard guard;
        volatile uint32_t sink = 0;
        bool completed = guard.Try([&] {
            sink = *reinterpret_cast<volatile uint32_t*>(r.libBase);
        });
        char d[48];
        std::snprintf(d, sizeof(d), "sink=0x%08x", (unsigned)sink);
        Add(r, "ScopedSigSegvGuard passes valid read through", completed, d);
    }
}

void RenderContent() noexcept {
    const Report& r = Run();

    ImGui::Text("UE runtime lib : %s", r.libFound ? "located" : "NOT FOUND (inject into game)");
    if (r.libFound) {
        ImGui::Text("base      : 0x%016lx", (unsigned long)r.libBase);
        ImGui::Text("span      : 0x%zx",    r.span);
        ImGui::TextWrapped("apkPath   : %s", r.apkPath.c_str());
    }
    ImGui::Separator();

    int passed = 0;
    for (const auto& c : r.checks) if (c.ok) ++passed;
    ImGui::Text("Checks: %d / %zu passed", passed, r.checks.size());

    if (ImGui::Button("Run SIGSEGV guard test (deliberately faults)")) {
        RunSigSegvTest();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(safe — fault is caught)");

    if (ImGui::BeginTable("checks", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();

        for (const auto& c : r.checks) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", c.label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImVec4 col = c.ok ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
                              : ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
            ImGui::TextColored(col, "%s", c.ok ? "PASS" : "FAIL");
            if (!c.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", c.detail.c_str());
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("logScroll", ImVec2(0, 180), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : r.log) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
}

}  // namespace SafeMemoryDiag
