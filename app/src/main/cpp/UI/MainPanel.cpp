// =============================================================================
// UI/MainPanel.cpp
// -----------------------------------------------------------------------------
// Single-window one-shot workflow:
//
//   [1] lib              — locate libUE4.so / libUnreal.so
//   [2] GUObject         — find GUObjectArray + Init()
//   [3] GNames           — find FNamePool + Init()
//   [4] Object Structure — UObject + UStruct + Advanced offset scans + Apply
//   [5] DUMP             — emit per-package C++ headers
//
// The DUMP button runs the chain in a background thread. Each step shows the
// current method/status; detailed traces go to logcat tag = "Dumper7".
// =============================================================================

#include "MainPanel.h"

#include "NameArray.h"
#include "ObjectArray.h"
#include "OffsetFinder.h"
#include "SafeMemory.h"
#include "SDKDumper.h"
#include "StructOffsetFinder.h"
#include "UEStructs.h"
#include "Utility.h"

#include "imgui.h"
#include "IconsFontAwesome7.h"

#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>

#include <dlfcn.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)
#define DLOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace MainPanel {

namespace {

enum class StepState { Pending, Running, Done, Failed };

// One row in a step's method chain. Pending = skipped (chain succeeded
// earlier), Done = this method found the target, Failed = tried but didn't.
struct MethodAttempt {
    std::string name;
    StepState   status = StepState::Pending;
    std::string note;   // optional one-liner (e.g. address found, candidates scanned)
};

struct Step {
    StepState                  state = StepState::Pending;
    std::string                summary;
    std::vector<MethodAttempt> methods;   // empty for steps without a method chain
};

Step       g_lib;
Step       g_gobjects;
Step       g_gnames;
Step       g_offsets;
UE4Info    g_ue4;
std::mutex g_stateMu;
std::atomic<bool> g_pipelineRunning{false};

struct IdaOverrides {
    bool filePresent = false;
    std::string path;
    bool hasGObjects = false;
    uintptr_t gobjects = 0;
    bool hasGNames = false;
    uintptr_t gnames = 0;
    bool hasGWorld = false;
    uintptr_t gworld = 0;
    bool hasProcessEvent = false;
    uintptr_t processEvent = 0;
    bool hasProcessEventVTableIndex = false;
    int32_t processEventVTableIndex = -1;
};


INTERNAL ImVec4 ColorFor(StepState s) noexcept {
    switch (s) {
        case StepState::Done:    return ImVec4(0.00f, 0.90f, 0.46f, 1.0f); // Glowing Neon Green
        case StepState::Failed:  return ImVec4(1.00f, 0.09f, 0.27f, 1.0f); // Neon Crimson Red
        case StepState::Running: return ImVec4(1.00f, 0.84f, 0.00f, 1.0f); // Bright Amber Gold
        default:                 return ImVec4(0.55f, 0.58f, 0.62f, 1.00f); // Muted Silver Gray
    }
}

INTERNAL const char* IconFor(StepState s) noexcept {
    switch (s) {
        case StepState::Done:    return ICON_FA_CHECK;
        case StepState::Failed:  return ICON_FA_XMARK;
        case StepState::Running: return ICON_FA_CIRCLE_NOTCH;
        default:                 return ICON_FA_CIRCLE;
    }
}

INTERNAL const char* FormatBytes(uint64_t bytes, char* out, size_t outSize) noexcept {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::snprintf(out, outSize, unit == 0 ? "%.0f %s" : "%.1f %s",
                  value, units[unit]);
    return out;
}

INTERNAL std::string FormatCodeRefProgress(const OffsetFinder::CodeRefProgress& p) noexcept {
    if (!p.active) return {};

    const double pct = p.totalBytes > 0
                     ? (100.0 * static_cast<double>(p.scannedBytes) /
                        static_cast<double>(p.totalBytes))
                     : 0.0;
    char scanned[32], total[32], segDone[32], segTotal[32], buf[192];
    std::snprintf(buf, sizeof(buf),
                  "%s %.1f%% — total %s/%s — chunk %u/%u %s/%s — refs %u",
                  p.targetLabel[0] ? p.targetLabel : "CodeRef",
                  pct,
                  FormatBytes(p.scannedBytes, scanned, sizeof(scanned)),
                  FormatBytes(p.totalBytes, total, sizeof(total)),
                  p.segmentIndex, p.segmentCount,
                  FormatBytes(p.segmentScannedBytes, segDone, sizeof(segDone)),
                  FormatBytes(p.segmentBytes, segTotal, sizeof(segTotal)),
                  p.candidatesScanned);
    return buf;
}

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

INTERNAL std::string Trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

INTERNAL std::string NormalizeKey(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '-' || c == '.' || c == ' ') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

INTERNAL bool ParseIdaInteger(std::string value, uint64_t& out) noexcept {
    value = Trim(std::move(value));
    while (!value.empty() &&
           (value.back() == ',' || value.back() == ';' || value.back() == '\'' ||
            value.back() == '"' || value.back() == '`')) {
        value.pop_back();
        value = Trim(std::move(value));
    }
    while (!value.empty() &&
           (value.front() == '\'' || value.front() == '"' || value.front() == '`')) {
        value.erase(value.begin());
        value = Trim(std::move(value));
    }
    if (value.empty()) return false;

    int base = 0;
    if (value.rfind("0x", 0) != 0 && value.rfind("0X", 0) != 0) {
        for (char c : value) {
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                base = 16;
                break;
            }
        }
    }

    char* end = nullptr;
    unsigned long long v = std::strtoull(value.c_str(), &end, base);
    if (end == value.c_str()) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

INTERNAL uintptr_t ResolveIdaAddress(uint64_t value) noexcept {
    if (value == 0) return 0;
    if (g_ue4.base != 0 && value >= g_ue4.base) {
        return static_cast<uintptr_t>(value);
    }
    return g_ue4.base + static_cast<uintptr_t>(value);
}

INTERNAL IdaOverrides LoadIdaOverrides() {
    IdaOverrides ov;
    ov.path = "/sdcard/Android/media/" + GetPackageName() + "/SDK/ida_offsets.txt";

    std::ifstream f(ov.path);
    if (!f.is_open()) return ov;
    ov.filePresent = true;

    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        size_t slash = line.find("//");
        size_t cut = std::min(hash == std::string::npos ? line.size() : hash,
                              slash == std::string::npos ? line.size() : slash);
        line.resize(cut);
        line = Trim(std::move(line));
        if (line.empty()) continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos) sep = line.find(':');
        if (sep == std::string::npos) continue;

        const std::string key = NormalizeKey(line.substr(0, sep));
        uint64_t value = 0;
        if (!ParseIdaInteger(line.substr(sep + 1), value)) continue;

        if (key == "gobjects" || key == "guobjectarray" || key == "gobjectarray" ||
            key == "objectarray" || key == "gobjectsoffset" || key == "guobjectarrayoffset") {
            ov.hasGObjects = true;
            ov.gobjects = static_cast<uintptr_t>(value);
        } else if (key == "gnames" || key == "namepool" || key == "namepooldata" ||
                   key == "fnamepool" || key == "gnameblocksdebug") {
            ov.hasGNames = true;
            ov.gnames = static_cast<uintptr_t>(value);
        } else if (key == "gworld" || key == "gworldoffset") {
            ov.hasGWorld = true;
            ov.gworld = static_cast<uintptr_t>(value);
        } else if (key == "processevent" || key == "pe" || key == "processeventoffset") {
            ov.hasProcessEvent = true;
            ov.processEvent = static_cast<uintptr_t>(value);
        } else if (key == "processeventvtableindex" || key == "pevtableindex" ||
                   key == "peindex" || key == "vtableindex") {
            ov.hasProcessEventVTableIndex = true;
            ov.processEventVTableIndex = static_cast<int32_t>(value);
        }
    }

    DLOGI("[ida] loaded overrides from %s: GObjects=%d GNames=%d GWorld=%d PE=%d PEIndex=%d",
          ov.path.c_str(),
          ov.hasGObjects,
          ov.hasGNames,
          ov.hasGWorld,
          ov.hasProcessEvent,
          ov.hasProcessEventVTableIndex);
    return ov;
}

INTERNAL const char* LibDisplayName(const UE4Info& info) noexcept {
    if (info.apkPath[0] == '\0') return "Unreal runtime lib";
    const char* slash = std::strrchr(info.apkPath, '/');
    return slash ? slash + 1 : info.apkPath;
}

INTERNAL void CopyDiscovery(char* dst, size_t dstSize, const std::string& src) noexcept {
    if (!dst || dstSize == 0) return;
    std::snprintf(dst, dstSize, "%s", src.c_str());
}

INTERNAL void CopyDiscovery(char* dst, size_t dstSize, const char* src) noexcept {
    if (!dst || dstSize == 0) return;
    std::snprintf(dst, dstSize, "%s", src ? src : "");
}

std::string g_dumpDir = "/sdcard/Android/media/" + GetPackageName() + "/SDK";

INTERNAL void ResetPipelineState() noexcept {
    std::lock_guard<std::mutex> lk(g_stateMu);
    g_lib = Step{};
    g_gobjects = Step{};
    g_gnames = Step{};
    g_offsets = Step{};
}

INTERNAL StepState ReadState(const Step& step) noexcept {
    std::lock_guard<std::mutex> lk(g_stateMu);
    return step.state;
}

INTERNAL void SetStepRunning(Step& step, const char* summary = nullptr) noexcept {
    std::lock_guard<std::mutex> lk(g_stateMu);
    step.state = StepState::Running;
    step.summary = summary ? summary : "";
    step.methods.clear();
}

INTERNAL void SetStepDone(Step& step, const std::string& summary) noexcept {
    std::lock_guard<std::mutex> lk(g_stateMu);
    step.state = StepState::Done;
    step.summary = summary;
}

INTERNAL void SetStepFailed(Step& step, const std::string& summary) noexcept {
    std::lock_guard<std::mutex> lk(g_stateMu);
    step.state = StepState::Failed;
    step.summary = summary;
}

INTERNAL std::string FormatUEVersion(const UEVersionInfo& v) noexcept {
    if (!v.ok) return "UE version unresolved";
    char b[96];
    if (v.patch >= 0) {
        std::snprintf(b, sizeof(b), "UE %d.%d.%d%s",
                      v.major, v.minor, v.patch,
                      v.exact ? "" : " (not exact)");
    } else {
        std::snprintf(b, sizeof(b), "UE %d.%d.x (not exact)",
                      v.major, v.minor);
    }
    return b;
}

INTERNAL void ApplyUEVersion(const UEVersionInfo& v) noexcept {
    CopyDiscovery(Off::Discovery::EngineVersionMethod,
                  sizeof(Off::Discovery::EngineVersionMethod), v.method);
    CopyDiscovery(Off::Discovery::EngineVersionDetail,
                  sizeof(Off::Discovery::EngineVersionDetail), v.detail);
    if (!v.ok) {
        Off::InSDK::EngineMajor = -1;
        Off::InSDK::EngineMinor = -1;
        Off::InSDK::EnginePatch = -1;
        Off::InSDK::EngineChangelist = 0;
        Off::Discovery::EngineVersion[0] = '\0';
        Off::Discovery::EngineBranch[0] = '\0';
        return;
    }

    Off::InSDK::EngineMajor = v.major;
    Off::InSDK::EngineMinor = v.minor;
    Off::InSDK::EnginePatch = v.patch;
    Off::InSDK::EngineChangelist = v.changelist;
    Off::InSDK::bIsUE5 = v.major >= 5;
    CopyDiscovery(Off::Discovery::EngineBranch,
                  sizeof(Off::Discovery::EngineBranch), v.branch);
    char version[32];
    if (v.patch >= 0) {
        std::snprintf(version, sizeof(version), "%d.%d.%d",
                      v.major, v.minor, v.patch);
    } else {
        std::snprintf(version, sizeof(version), "%d.%d.x",
                      v.major, v.minor);
    }
    CopyDiscovery(Off::Discovery::EngineVersion,
                  sizeof(Off::Discovery::EngineVersion), version);
}

INTERNAL void ApplyIdaExtraOverrides() noexcept {
    IdaOverrides ov = LoadIdaOverrides();
    if (!ov.filePresent) return;

    if (ov.hasGWorld) {
        Off::Resolved::GWorld = ResolveIdaAddress(ov.gworld);
        CopyDiscovery(Off::Discovery::GWorldMethod,
                      sizeof(Off::Discovery::GWorldMethod), "ida-manual");
        char detail[160];
        std::snprintf(detail, sizeof(detail), "%s raw=0x%lx",
                      ov.path.c_str(), static_cast<unsigned long>(ov.gworld));
        CopyDiscovery(Off::Discovery::GWorldDetail,
                      sizeof(Off::Discovery::GWorldDetail), detail);
        DLOGI("[ida] GWorld raw=0x%lx -> 0x%lx",
              static_cast<unsigned long>(ov.gworld),
              static_cast<unsigned long>(Off::Resolved::GWorld));
    }

    if (ov.hasProcessEvent) {
        Off::Resolved::ProcessEvent = ResolveIdaAddress(ov.processEvent);
        CopyDiscovery(Off::Discovery::ProcessEventMethod,
                      sizeof(Off::Discovery::ProcessEventMethod), "ida-manual");
        char detail[160];
        std::snprintf(detail, sizeof(detail), "%s raw=0x%lx",
                      ov.path.c_str(), static_cast<unsigned long>(ov.processEvent));
        CopyDiscovery(Off::Discovery::ProcessEventDetail,
                      sizeof(Off::Discovery::ProcessEventDetail), detail);
        DLOGI("[ida] ProcessEvent raw=0x%lx -> 0x%lx",
              static_cast<unsigned long>(ov.processEvent),
              static_cast<unsigned long>(Off::Resolved::ProcessEvent));
    }

    if (ov.hasProcessEventVTableIndex) {
        Off::Resolved::ProcessEventVTableIndex = ov.processEventVTableIndex;
        if (!ov.hasProcessEvent) {
            CopyDiscovery(Off::Discovery::ProcessEventMethod,
                          sizeof(Off::Discovery::ProcessEventMethod),
                          "ida-vtable-index");
        }
        char detail[160];
        std::snprintf(detail, sizeof(detail), "%s vtableIndex=0x%x",
                      ov.path.c_str(), ov.processEventVTableIndex);
        CopyDiscovery(Off::Discovery::ProcessEventDetail,
                      sizeof(Off::Discovery::ProcessEventDetail), detail);
        DLOGI("[ida] ProcessEvent vtable index=0x%x",
              ov.processEventVTableIndex);
    }
}

// ---- Step bodies -----------------------------------------------------------

INTERNAL bool RunLib() noexcept {
    SetStepRunning(g_lib, "locating libUE4.so / libUnreal.so");
    g_ue4 = FindAndOpenLibUE4();
    if (g_ue4.base != 0) {
        Off::Resolved::LibBase = g_ue4.base;
        SafeMemory::Init(g_ue4);
        CopyDiscovery(Off::Discovery::LibPath,
                      sizeof(Off::Discovery::LibPath), g_ue4.apkPath);
        UEVersionInfo version = FindUEVersion(g_ue4);
        ApplyUEVersion(version);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "%s @ 0x%lx — %s (%s)",
                      LibDisplayName(g_ue4),
                      static_cast<unsigned long>(g_ue4.base),
                      FormatUEVersion(version).c_str(),
                      version.method[0] ? version.method : "unresolved");
        SetStepDone(g_lib, buf);
        return true;
    } else {
        SetStepFailed(g_lib, "libUE4.so / libUnreal.so not loaded in this process");
        return false;
    }
}

// Run a method-chain in order. Each method's per-attempt status is recorded
// so the UI can show which one actually succeeded vs which were skipped.
// Stops at first success — remaining methods stay StepState::Pending.
template <class Fn>
INTERNAL OffsetFinder::Result RunMethodChain(
        Step& step,
        std::initializer_list<std::pair<const char*, Fn>> methods) noexcept {
    {
        std::lock_guard<std::mutex> lk(g_stateMu);
        step.methods.clear();
        step.methods.reserve(methods.size());
        for (auto& m : methods) {
            step.methods.push_back({m.first, StepState::Pending, {}});
        }
    }

    OffsetFinder::Result winner;
    int idx = 0;
    for (auto& m : methods) {
        {
            std::lock_guard<std::mutex> lk(g_stateMu);
            step.methods[idx].status = StepState::Running;
            step.summary = std::string("trying ") + m.first;
        }
        auto r = m.second();
        if (r.ok) {
            char note[96];
            std::snprintf(note, sizeof(note), "@ 0x%lx (%lu candidates, %.1f ms)",
                          static_cast<unsigned long>(r.address),
                          static_cast<unsigned long>(r.candidatesScanned),
                          r.elapsedMicros / 1000.0);
            {
                std::lock_guard<std::mutex> lk(g_stateMu);
                step.methods[idx].status = StepState::Done;
                step.methods[idx].note = note;
            }
            winner = r;
            return winner;
        }
        char note[96];
        std::snprintf(note, sizeof(note), "miss (%lu candidates, %.1f ms)",
                      static_cast<unsigned long>(r.candidatesScanned),
                      r.elapsedMicros / 1000.0);
        {
            std::lock_guard<std::mutex> lk(g_stateMu);
            step.methods[idx].status = StepState::Failed;
            step.methods[idx].note = note;
        }
        ++idx;
    }
    return winner;
}

INTERNAL bool RunGObjects() noexcept {
    SetStepRunning(g_gobjects, "finding GUObjectArray");

    IdaOverrides ov = LoadIdaOverrides();
    if (ov.filePresent && ov.hasGObjects) {
        const uintptr_t addr = ResolveIdaAddress(ov.gobjects);
        DLOGI("[ida] trying GUObjectArray raw=0x%lx -> 0x%lx",
              static_cast<unsigned long>(ov.gobjects),
              static_cast<unsigned long>(addr));

        if (!UE::ObjectArray::Init(addr)) {
            char fail[160];
            std::snprintf(fail, sizeof(fail),
                          "IDA GUObjectArray rejected @ 0x%lx",
                          static_cast<unsigned long>(addr));
            SetStepFailed(g_gobjects, fail);
            return false;
        }

        Off::Resolved::GObjects = addr;
        CopyDiscovery(Off::Discovery::GObjectsMethod,
                      sizeof(Off::Discovery::GObjectsMethod), "ida-manual");
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s raw=0x%lx",
                      ov.path.c_str(), static_cast<unsigned long>(ov.gobjects));
        CopyDiscovery(Off::Discovery::GObjectsDetail,
                      sizeof(Off::Discovery::GObjectsDetail), detail);

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "ida-manual @ 0x%lx — %d objects",
                      static_cast<unsigned long>(addr),
                      UE::ObjectArray::Num());
        SetStepDone(g_gobjects, buf);
        return true;
    }

    using Fn = std::function<OffsetFinder::Result()>;
    auto r = RunMethodChain<Fn>(g_gobjects, {
        {"Symbol",    [&] { return OffsetFinder::FindGObjects_Symbol   (g_ue4); }},
        {"StringRef", [&] { return OffsetFinder::FindGObjects_StringRef(g_ue4); }},
        {"CodeRefQuick", [&] { return OffsetFinder::FindGObjects_CodeRefQuick(g_ue4); }},
        {"CodeRef",   [&] { return OffsetFinder::FindGObjects_CodeRef  (g_ue4); }},
        {"BssScan",   [&] { return OffsetFinder::FindGObjects_BssScan  (g_ue4); }},
        {"DeepScan",  [&] { return OffsetFinder::FindGObjects_DeepScan (g_ue4); }},
    });

    if (!r.ok) {
        SetStepFailed(g_gobjects, "all methods failed (see logcat for traces)");
        return false;
    }
    if (!UE::ObjectArray::Init(r.address)) {
        SetStepFailed(g_gobjects, "Init() rejected the address (Probe failed)");
        return false;
    }
    Off::Resolved::GObjects = r.address;
    CopyDiscovery(Off::Discovery::GObjectsMethod,
                  sizeof(Off::Discovery::GObjectsMethod),
                  OffsetFinder::MethodName(r.method));
    CopyDiscovery(Off::Discovery::GObjectsDetail,
                  sizeof(Off::Discovery::GObjectsDetail), r.detail);
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "%s @ 0x%lx — %d objects",
                  OffsetFinder::MethodName(r.method),
                  static_cast<unsigned long>(r.address),
                  UE::ObjectArray::Num());
    SetStepDone(g_gobjects, buf);
    return true;
}

INTERNAL bool RunGNames() noexcept {
    SetStepRunning(g_gnames, "finding GNames / NamePoolData");

    IdaOverrides ov = LoadIdaOverrides();
    if (ov.filePresent && ov.hasGNames) {
        const uintptr_t addr = ResolveIdaAddress(ov.gnames);
        DLOGI("[ida] trying GNames raw=0x%lx -> 0x%lx",
              static_cast<unsigned long>(ov.gnames),
              static_cast<unsigned long>(addr));

        if (!UE::NameArray::Init(addr)) {
            char fail[160];
            std::snprintf(fail, sizeof(fail),
                          "IDA GNames/NamePool rejected @ 0x%lx",
                          static_cast<unsigned long>(addr));
            SetStepFailed(g_gnames, fail);
            return false;
        }

        Off::Resolved::GNames = addr;
        CopyDiscovery(Off::Discovery::GNamesMethod,
                      sizeof(Off::Discovery::GNamesMethod), "ida-manual");
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s raw=0x%lx",
                      ov.path.c_str(), static_cast<unsigned long>(ov.gnames));
        CopyDiscovery(Off::Discovery::GNamesDetail,
                      sizeof(Off::Discovery::GNamesDetail), detail);

        std::string first = UE::NameArray::GetName(0);
        if (first.empty()) first = "?";
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "ida-manual @ 0x%lx — first=\"%s\"",
                      static_cast<unsigned long>(addr),
                      first.c_str());
        SetStepDone(g_gnames, buf);
        return true;
    }

    using Fn = std::function<OffsetFinder::Result()>;
    auto r = RunMethodChain<Fn>(g_gnames, {
        {"Symbol",    [&] { return OffsetFinder::FindGNames_Symbol   (g_ue4); }},
        {"FuncWalk",  [&] { return OffsetFinder::FindGNames_FuncWalk (g_ue4); }},
        {"StringRef", [&] { return OffsetFinder::FindGNames_StringRef(g_ue4); }},
        {"CodeRefQuick", [&] { return OffsetFinder::FindGNames_CodeRefQuick(g_ue4); }},
        {"CodeRef",   [&] { return OffsetFinder::FindGNames_CodeRef  (g_ue4); }},
        {"BssScan",   [&] { return OffsetFinder::FindGNames_BssScan  (g_ue4); }},
    });

    if (!r.ok) {
        SetStepFailed(g_gnames, "all methods failed (see logcat for traces)");
        return false;
    }
    if (!UE::NameArray::Init(r.address)) {
        SetStepFailed(g_gnames, "Init() rejected the address (Probe failed)");
        return false;
    }
    Off::Resolved::GNames = r.address;
    CopyDiscovery(Off::Discovery::GNamesMethod,
                  sizeof(Off::Discovery::GNamesMethod),
                  OffsetFinder::MethodName(r.method));
    CopyDiscovery(Off::Discovery::GNamesDetail,
                  sizeof(Off::Discovery::GNamesDetail), r.detail);
    std::string first = UE::NameArray::GetName(0);
    if (first.empty()) first = "?";
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "%s @ 0x%lx — first=\"%s\"",
                  OffsetFinder::MethodName(r.method),
                  static_cast<unsigned long>(r.address),
                  first.c_str());
    SetStepDone(g_gnames, buf);
    return true;
}

INTERNAL bool RunOffsets() noexcept {
    SetStepRunning(g_offsets, "scanning UObject layout");

    auto uobj = StructOffsetFinder::FindUObjectLayout();
    if (!uobj.ok) {
        SetStepFailed(g_offsets, "UObject scan failed (see [uobjoff] in logcat)");
        return false;
    }
    StructOffsetFinder::Apply(uobj);

    SetStepRunning(g_offsets, "scanning UStruct layout");
    auto ustruct = StructOffsetFinder::FindUStructLayout();
    if (!ustruct.ok) {
        SetStepFailed(g_offsets, "UStruct scan failed (see [ustructoff] in logcat)");
        return false;
    }
    StructOffsetFinder::Apply(ustruct);

    SetStepRunning(g_offsets, "scanning advanced property layout");
    auto adv = StructOffsetFinder::FindAdvancedLayout();
    if (!adv.ok) {
        SetStepFailed(g_offsets, "Advanced scan failed (see [advoff] in logcat)");
        return false;
    }
    StructOffsetFinder::Apply(adv);

    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "Cls=0x%x Name=0x%x Super=0x%x Children=0x%x CDO=0x%x Func=0x%x",
                  Off::UObject::ClassPrivate, Off::UObject::NamePrivate,
                  Off::UStruct::SuperStruct,  Off::UStruct::Children,
                  Off::UClass::ClassDefaultObject, Off::UFunction::Func);
    SetStepDone(g_offsets, buf);
    return true;
}

INTERNAL bool RunDump() noexcept {
    return SDKDumper::Start(g_dumpDir);
}

INTERNAL void PipelineEntry() noexcept {
    ResetPipelineState();

    DLOGI("[pipeline] start package=%s", GetPackageName().c_str());

    bool ok = RunLib();
    if (ok) ApplyIdaExtraOverrides();
    if (ok) ok = RunGObjects();
    if (ok) ok = RunGNames();
    if (ok) ok = RunOffsets();
    if (ok && !RunDump()) {
        SetStepFailed(g_offsets, "SDK dump worker refused to start");
    }

    g_pipelineRunning.store(false);
}

INTERNAL void StartPipeline() noexcept {
    if (g_pipelineRunning.load() || SDKDumper::IsRunning()) return;
    g_pipelineRunning.store(true);
    std::thread(PipelineEntry).detach();
}

// ---- Per-step rendering ----------------------------------------------------

INTERNAL void RenderStep(int n, const char* name, const Step& s) noexcept {
    ImVec4 col = ColorFor(s.state);
    
    // Draw step container inside a clean child panel
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    
    float height = 44.0f;
    if (!s.summary.empty()) height += 20.0f;
    if (!s.methods.empty()) height += (s.methods.size() * 26.0f) + 16.0f;
    
    char childId[64];
    std::snprintf(childId, sizeof(childId), "step_child_%d", n);
    
    ImGui::PushStyleColor(ImGuiCol_Border, col);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 0.35f));
    
    ImGui::BeginChild(childId, ImVec2(0.0f, height), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    
    ImGui::TextColored(col, "%s  Step %d: %s", IconFor(s.state), n, name);
    
    if (!s.summary.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
        ImGui::TextDisabled("%s", s.summary.c_str());
    }
    
    if (!s.methods.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
        ImGui::Separator();
        ImGui::Spacing();
        
        if (ImGui::BeginTable("methods_table", 3, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
            
            for (const auto& m : s.methods) {
                ImGui::TableNextRow();
                ImVec4 mcol = ColorFor(m.status);
                
                ImGui::TableNextColumn();
                ImGui::TextColored(mcol, " %s %s", IconFor(m.status), m.name.c_str());
                
                ImGui::TableNextColumn();
                const char* tag = "skipped";
                switch (m.status) {
                    case StepState::Done:    tag = "success"; break;
                    case StepState::Failed:  tag = "failed";  break;
                    case StepState::Running: tag = "running..."; break;
                    default: break;
                }
                ImGui::TextColored(mcol, "%s", tag);
                
                ImGui::TableNextColumn();
                std::string note = m.note;
                if (m.status == StepState::Running &&
                    (m.name == "CodeRef" || m.name == "CodeRefQuick")) {
                    std::string progress =
                            FormatCodeRefProgress(OffsetFinder::CodeRefSnapshot());
                    if (!progress.empty()) note = progress;
                }
                if (!note.empty()) {
                    ImGui::TextDisabled("%s", note.c_str());
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            ImGui::EndTable();
        }
    }
    
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
}

}  // namespace

void Render() noexcept {

    ImGui::SetNextWindowSize({900.0f,500.0f},ImGuiCond_Once);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    
    char title[128];
    std::snprintf(title, sizeof(title), " %s  Dumper-7 [Android]", ICON_FA_MICROCHIP);
    if (!ImGui::Begin(title)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.00f, 0.75f, 1.00f, 1.0f));
    ImGui::Text("%s  DUMPER-7 ENGINE", ICON_FA_MICROCHIP);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("|  Universal Unreal Engine SDK Generator");
    ImGui::Separator();
    ImGui::Spacing();

    bool pipelineRunning = g_pipelineRunning.load();
    bool sdkRunning = SDKDumper::IsRunning();

    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 DisplaySize = io.DisplaySize;
    float sidebarWidth = contentSize.x * 0.4f;
    float mainWidth = contentSize.x - sidebarWidth - 12.0f;

    ImGui::BeginChild("SidebarPanel", ImVec2(sidebarWidth, contentSize.y - 10.0f), true, ImGuiWindowFlags_NoScrollbar);
    
    ImGui::TextDisabled("CONFIGURATION");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Direct Calls", &SDKDumper::g_useDirectCalls);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Use native function pointers instead of ProcessEvent.\n"
                          "Faster but only works for C++ (non-Blueprint) functions.");
    }
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextDisabled("TARGET OUTPUT");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.14f, 0.18f, 0.50f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    
    char dumpDirBuf[256];
    std::snprintf(dumpDirBuf, sizeof(dumpDirBuf), "%s", g_dumpDir.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##TargetDir", dumpDirBuf, sizeof(dumpDirBuf), ImGuiInputTextFlags_ReadOnly);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The folder on the device where target C++ SDK headers will be generated.");
    }
    
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    if (pipelineRunning || sdkRunning) ImGui::BeginDisabled();
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.45f, 0.74f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.55f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.65f, 1.00f, 1.00f));
    
    char buttonLabel[64];
    std::snprintf(buttonLabel, sizeof(buttonLabel), "%s  DUMP SDK", ICON_FA_PLAY);
    if (ImGui::Button(buttonLabel, ImVec2(sidebarWidth - 16.0f, 40.0f))) {
        StartPipeline();
    }
    
    ImGui::PopStyleColor(3);
    
    if (pipelineRunning || sdkRunning) ImGui::EndDisabled();

    if (pipelineRunning || sdkRunning) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
        ImGui::TextColored(ImVec4(1.00f, 0.84f, 0.00f, 1.0f), "%s Running...", ICON_FA_CIRCLE_NOTCH);
    }
    
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("MainStatusPanel", ImVec2(mainWidth, contentSize.y - 10.0f), false);
    
    Step lib;
    Step gobjects;
    Step gnames;
    Step offsets;
    {
        std::lock_guard<std::mutex> lk(g_stateMu);
        lib = g_lib;
        gobjects = g_gobjects;
        gnames = g_gnames;
        offsets = g_offsets;
    }

    // Auto-scroll logic: detect transitions to newer steps to keep the focus on active work
    static int lastActiveStep = 0;
    int currentActiveStep = 0;
    if (lib.state == StepState::Running || lib.state == StepState::Done) currentActiveStep = 1;
    if (gobjects.state == StepState::Running || gobjects.state == StepState::Done) currentActiveStep = 2;
    if (gnames.state == StepState::Running || gnames.state == StepState::Done) currentActiveStep = 3;
    if (offsets.state == StepState::Running || offsets.state == StepState::Done) currentActiveStep = 4;
    if (sdkRunning) currentActiveStep = 5;

    bool triggerScroll = (currentActiveStep > lastActiveStep);
    lastActiveStep = currentActiveStep;

    if (!pipelineRunning && !sdkRunning) {
        lastActiveStep = 0;
    }

    RenderStep(1, "Lib & Engine Version Loader", lib);
    RenderStep(2, "GUObject Scanner", gobjects);
    RenderStep(3, "GNames Reflection Parser", gnames);
    RenderStep(4, "Object Properties Alignment", offsets);

    {
        auto snap = SDKDumper::Snapshot();
        bool running = sdkRunning;
        bool chainFailed = lib.state == StepState::Failed ||
                           gobjects.state == StepState::Failed ||
                           gnames.state == StepState::Failed ||
                           offsets.state == StepState::Failed;

        StepState ds = StepState::Pending;
        if (running)                                       ds = StepState::Running;
        else if (chainFailed)                              ds = StepState::Failed;
        else if (snap.phase == SDKDumper::Phase::Done)     ds = StepState::Done;
        else if (snap.phase == SDKDumper::Phase::Failed)   ds = StepState::Failed;
        else if (snap.phase == SDKDumper::Phase::Cancelled) ds = StepState::Failed;

        ImVec4 col = ColorFor(ds);
        
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, col);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 0.35f));
        
        float height = 48.0f;
        if (running) height += 48.0f;
        else if (ds == StepState::Done) height += 20.0f;
        else if (ds == StepState::Failed) height += 20.0f;
        
        ImGui::BeginChild("step_child_5", ImVec2(0.0f, height), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        
        ImGui::TextColored(col, "%s  Step 5: Code SDK Emission", IconFor(ds));
        
        if (running) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
            ImGui::TextColored(col, "%s  -  written %d/%d packages",
                               SDKDumper::PhaseName(snap.phase),
                               snap.writtenPackages, snap.totalPackages);
            
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
            ImGui::TextDisabled("Classes: %d  |  Structs: %d  |  Enums: %d  |  Functions: %d",
                               snap.classCount, snap.structCount, snap.enumCount, snap.functionCount);
                               
            ImGui::Spacing();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
            
            float pct = 0.0f;
            if (snap.totalPackages > 0 && snap.phase == SDKDumper::Phase::Writing) {
                pct = (float)snap.writtenPackages / (float)snap.totalPackages;
            } else if (snap.totalObjects > 0) {
                pct = (float)snap.processedObjects / (float)snap.totalObjects;
            }
            
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.00f, 0.75f, 1.00f, 1.00f));
            ImGui::ProgressBar(pct, ImVec2(mainWidth - 48.0f, 12.0f), "");
            ImGui::PopStyleColor();
            
        } else if (ds == StepState::Done) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
            ImGui::TextColored(col, "Done in %.2fs  |  %d Packages  |  %d Classes",
                               snap.elapsedMicros / 1e6,
                               snap.writtenPackages, snap.classCount);
        } else if (ds == StepState::Failed) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
            const char* msg = chainFailed
                            ? "Skipped because an earlier step failed."
                            : snap.phase == SDKDumper::Phase::Cancelled
                            ? "Cancelled."
                            : snap.error.c_str();
            ImGui::TextColored(col, "%s", msg);
        }
        
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
    
    bool isScrollingNearBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 25.0f;
    if (triggerScroll || ((pipelineRunning || sdkRunning) && isScrollingNearBottom)) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();

    ImGui::End();
}

}  // namespace MainPanel
