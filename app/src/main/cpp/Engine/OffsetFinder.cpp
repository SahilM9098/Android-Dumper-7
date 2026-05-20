// =============================================================================
// Engine/OffsetFinder.cpp
// -----------------------------------------------------------------------------
// Three strategies for locating GUObjectArray inside a stripped libUE4.so:
//
//   * Symbol strategy
//       Try a list of likely names via FindSymbolDynamic / FindSymbolDisk.
//       Each candidate is validated with ObjectArray::Probe — a stale or
//       wrong-typed symbol cannot lock us in.
//
//   * String-ref strategy (Dumper-7 style, no symbols required)
//       1) Locate a known UE literal (e.g. "GUObjectArray", "FUObjectArray",
//          "MaxObjectsNotConsideredByGC") in libUE4 .rodata.
//       2) Scan executable segments for ADRP+ADD pairs that resolve to the
//          literal — these are call-site references.
//       3) From each reference site, walk a window of nearby instructions
//          tracking per-register ADRP targets; whenever an ADD/LDR resolves
//          to a writable-segment address, run ObjectArray::ProbeNoGuard. The
//          first probe that passes is GUObjectArray.
//
//   * BSS scan strategy
//       Last-resort exhaustive walk of libUE4's writable PT_LOAD segments
//       at 8-byte stride. Cheap to run because Probe rejects almost every
//       candidate in 1-2 reads, and the vtable-in-libUE4 check inside
//       Probe makes false positives effectively impossible.
//
// Each strategy has its own FindGObjects_* entrypoint. FindGObjects() chains
// them in priority order.
// =============================================================================

#include "OffsetFinder.h"

#include "NameArray.h"
#include "ObjectArray.h"
#include "PatternScanner.h"
#include "SafeMemory.h"
#include "UEStructs.h"

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <link.h>
#include <mutex>
#include <string>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)
#define DLOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace OffsetFinder {

namespace {

// =============================================================================
// Common helpers
// =============================================================================

constexpr uintptr_t kScanStride          = sizeof(void*);
constexpr size_t    kMinSegmentTailBytes = 0x40;
constexpr int       kMaxStringRefs       = 256;
constexpr int       kStringRefForward    = 512;   // instructions
constexpr int       kStringRefBackward   = 512;

std::mutex       g_codeRefProgressMu;
CodeRefProgress g_codeRefProgress;

struct Segment {
    uintptr_t start;
    uintptr_t end;
    uint32_t  flags;
};

INTERNAL uint64_t SegmentBytes(const Segment& seg) noexcept {
    return seg.end > seg.start ? static_cast<uint64_t>(seg.end - seg.start) : 0;
}

INTERNAL void CopySmall(char* dst, size_t dstSize, const char* src) noexcept {
    if (!dst || dstSize == 0) return;
    std::snprintf(dst, dstSize, "%s", src ? src : "");
}

INTERNAL void UpdateCodeRefProgress(const char* targetLabel,
                                    const char* stage,
                                    uint32_t segmentIndex,
                                    uint32_t segmentCount,
                                    uint64_t totalBytes,
                                    uint64_t scannedBytes,
                                    uint64_t segmentBytes,
                                    uint64_t segmentScannedBytes,
                                    uint32_t candidatesScanned) noexcept {
    std::lock_guard<std::mutex> lk(g_codeRefProgressMu);
    g_codeRefProgress.active = true;
    CopySmall(g_codeRefProgress.targetLabel,
              sizeof(g_codeRefProgress.targetLabel), targetLabel);
    CopySmall(g_codeRefProgress.stage,
              sizeof(g_codeRefProgress.stage), stage);
    g_codeRefProgress.segmentIndex = segmentIndex;
    g_codeRefProgress.segmentCount = segmentCount;
    g_codeRefProgress.totalBytes = totalBytes;
    g_codeRefProgress.scannedBytes = scannedBytes;
    g_codeRefProgress.segmentBytes = segmentBytes;
    g_codeRefProgress.segmentScannedBytes = segmentScannedBytes;
    g_codeRefProgress.candidatesScanned = candidatesScanned;
}

INTERNAL void FinishCodeRefProgress(const char* targetLabel,
                                    const char* stage,
                                    uint64_t totalBytes,
                                    uint64_t scannedBytes,
                                    uint32_t candidatesScanned) noexcept {
    std::lock_guard<std::mutex> lk(g_codeRefProgressMu);
    CopySmall(g_codeRefProgress.targetLabel,
              sizeof(g_codeRefProgress.targetLabel), targetLabel);
    CopySmall(g_codeRefProgress.stage,
              sizeof(g_codeRefProgress.stage), stage);
    g_codeRefProgress.scannedBytes = scannedBytes;
    g_codeRefProgress.totalBytes = totalBytes;
    g_codeRefProgress.segmentScannedBytes = g_codeRefProgress.segmentBytes;
    g_codeRefProgress.candidatesScanned = candidatesScanned;
    g_codeRefProgress.active = false;
}

INTERNAL std::vector<Segment> CollectSegments(uintptr_t base, uint32_t requiredFlags,
                                              uint32_t forbiddenFlags) noexcept {
    std::vector<Segment> out;
    struct Ctx { uintptr_t base; uint32_t req; uint32_t forbid; std::vector<Segment>* out; };
    Ctx ctx{ base, requiredFlags, forbiddenFlags, &out };

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        if (info->dlpi_addr != c->base) return 0;
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& p = info->dlpi_phdr[i];
            if (p.p_type != PT_LOAD) continue;
            if ((p.p_flags & c->req) != c->req) continue;
            if ((p.p_flags & c->forbid) != 0)   continue;
            Segment s;
            s.start = info->dlpi_addr + p.p_vaddr;
            s.end   = s.start + p.p_memsz;
            s.flags = p.p_flags;
            c->out->push_back(s);
        }
        return 1;
    }, &ctx);
    return out;
}

// Variant that does NOT filter on dlpi_addr — collects matching segments
// across every loaded shared object. Used by the DeepScan finder so we can
// catch GUObjectArray when it lives in a sibling library (e.g. UE5
// libCoreUObject.so split out from libUnreal.so).
INTERNAL std::vector<Segment> CollectSegmentsAllLibs(uint32_t requiredFlags,
                                                     uint32_t forbiddenFlags) noexcept {
    std::vector<Segment> out;
    struct Ctx { uint32_t req; uint32_t forbid; std::vector<Segment>* out; };
    Ctx ctx{ requiredFlags, forbiddenFlags, &out };

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& p = info->dlpi_phdr[i];
            if (p.p_type != PT_LOAD) continue;
            if ((p.p_flags & c->req) != c->req) continue;
            if ((p.p_flags & c->forbid) != 0)   continue;
            Segment s;
            s.start = info->dlpi_addr + p.p_vaddr;
            s.end   = s.start + p.p_memsz;
            s.flags = p.p_flags;
            c->out->push_back(s);
        }
        return 0;  // continue iterating
    }, &ctx);
    return out;
}

INTERNAL bool AddressInSegments(uintptr_t addr, const std::vector<Segment>& segs) noexcept {
    for (const auto& s : segs) {
        if (addr >= s.start && addr < s.end) return true;
    }
    return false;
}

INTERNAL bool PageCanReachSegments(uintptr_t page,
                                   const std::vector<Segment>& segs,
                                   uintptr_t maxDelta) noexcept {
    uintptr_t last = UINTPTR_MAX - page < maxDelta ? UINTPTR_MAX : page + maxDelta;
    for (const auto& s : segs) {
        if (page <= s.end && last >= s.start) return true;
    }
    return false;
}

// Validator: takes a writable-segment candidate, returns the address Init
// should be called with (may be addr itself or addr + wrapper-offset for
// targets that have an outer struct), or 0 on rejection.
//
// IMPORTANT: callers must already have a ScopedSigSegvGuard installed —
// validators dereference the candidate, which faults on almost-but-not-real
// candidates and longjmps out of the enclosing Try.
using Validator = uintptr_t (*)(uintptr_t);

INTERNAL uintptr_t ValidateGObjectsCandidate(uintptr_t addr) noexcept {
    if (UE::ObjectArray::ProbeNoGuard(addr)) return addr;
    if (UE::ObjectArray::ProbeNoGuard(addr + Off::FUObjectArray::ObjObjects)) return addr;
    return 0;
}

// FNamePool / GNames validator. NameArray::ProbeNoGuard handles all three
// modes internally (direct offset / *source-as-Blocks / brute scan), so
// there's no wrapper-offset retry layer here.
INTERNAL uintptr_t ValidateGNamesCandidate(uintptr_t addr) noexcept {
    return UE::NameArray::ProbeNoGuard(addr) ? addr : 0;
}

// Make sure SafeMemory has been populated before a finder runs. The Probe
// validation needs both libUE4 segments (for SafeRead bounds) and
// cross-library executable segments (for the vtable check). Without this,
// IsExecutable returns false and every Probe rejects every candidate even
// on a healthy build — the bug pattern that broke all methods after the
// deep-scan changes when the user went straight to the ObjectArray tab.
INTERNAL void EnsureInitialized(const UE4Info& info) noexcept {
    // Always re-init; SafeMemory::Init is idempotent and re-enumerates
    // dl_iterate_phdr each time, picking up any libraries the host loaded
    // since our last refresh. UObject vtables on UE5 split-module builds
    // commonly live in a sibling library that loads lazily.
    if (info.base != 0) {
        SafeMemory::Init(info);
    }
}

// =============================================================================
// AArch64 instruction decoders (used by the string-ref strategy)
// =============================================================================

INTERNAL bool is_adrp(uint32_t ins) noexcept {
    return (ins & 0x9F000000) == 0x90000000;
}

// ADR (page-relative, single instruction, ±1 MB range). Same encoding as
// ADRP but bit 31 = 0. Some compilers emit ADR instead of ADRP+ADD when
// the target is close enough — common for strings in nearby .rodata.
INTERNAL bool is_adr(uint32_t ins) noexcept {
    return (ins & 0x9F000000) == 0x10000000;
}

// ADR resolves directly: result = pc + sign_extend(immhi:immlo, 21).
INTERNAL uintptr_t adr_target(uintptr_t pc, uint32_t ins) noexcept {
    uint32_t immlo = (ins >> 29) & 0x3;
    uint32_t immhi = (ins >> 5)  & 0x7FFFF;
    int64_t  imm   = static_cast<int64_t>((immhi << 2) | immlo);
    if (imm & (1LL << 20)) imm |= ~((1LL << 21) - 1);
    return pc + imm;
}

// ADD (immediate, 64-bit) — used by ADRP+ADD address materialization.
INTERNAL bool is_add_imm64(uint32_t ins) noexcept {
    return (ins & 0xFF800000) == 0x91000000;
}

// LDR (immediate, unsigned offset, 64-bit) — used by ADRP+LDR indirect refs.
INTERNAL bool is_ldr_imm64(uint32_t ins) noexcept {
    return (ins & 0xFFC00000) == 0xF9400000;
}

INTERNAL uint32_t reg_rd(uint32_t ins) noexcept { return ins & 0x1F; }
INTERNAL uint32_t reg_rn(uint32_t ins) noexcept { return (ins >> 5) & 0x1F; }

INTERNAL uintptr_t adrp_target(uintptr_t pc, uint32_t ins) noexcept {
    uint32_t immlo = (ins >> 29) & 0x3;
    uint32_t immhi = (ins >> 5)  & 0x7FFFF;
    int64_t  imm   = static_cast<int64_t>((immhi << 2) | immlo);
    if (imm & (1LL << 20)) imm |= ~((1LL << 21) - 1);  // sign-extend 21 bits
    return (pc & ~uintptr_t{0xFFF}) + (imm << 12);
}

INTERNAL uint32_t add_imm12(uint32_t ins) noexcept {
    uint32_t shift = (ins >> 22) & 1;            // sh: 0 = LSL #0, 1 = LSL #12
    uint32_t imm   = (ins >> 10) & 0xFFF;
    return shift ? (imm << 12) : imm;
}

INTERNAL uint32_t ldr_imm12_scale8(uint32_t ins) noexcept {
    uint32_t imm = (ins >> 10) & 0xFFF;
    return imm * 8;                               // 64-bit LDR scales by 8
}

// =============================================================================
// Symbol strategy
// =============================================================================

struct SymbolName {
    const char* name;
    const char* note;
};

constexpr SymbolName kGObjectsSymbols[] = {
    { "GUObjectArray",                       "root-namespace global"        },
    { "_Z13GUObjectArray",                   "alt root mangling"            },
    { "_ZN12FUObjectArray13GUObjectArrayE",  "FUObjectArray::GUObjectArray" },
    { "ObjectsArray",                        "very old engine spelling"     },
};

// Outcome of a single symbol attempt. Drives the per-attempt trace lines that
// FindGObjects_Symbol publishes back through Result.traceLines.
struct SymbolAttempt {
    const char* name      = nullptr;
    bool        resolved  = false;
    bool        fromDisk  = false;
    uintptr_t   addr      = 0;
    bool        faulted   = false;
    bool        validated = false;
    std::vector<std::string> probeLines;  // populated by ProbeReport on failure
};

INTERNAL SymbolAttempt TrySymbolDetailed(const UE4Info& info, const char* name) noexcept {
    SymbolAttempt a;
    a.name = name;

    void* sym = FindSymbolDynamic(info.base, name);
    if (sym == nullptr && info.apkPath[0] != 0) {
        sym = FindSymbolDisk(info.apkPath, info.base, name);
        if (sym != nullptr) a.fromDisk = true;
    }
    if (sym == nullptr) return a;

    a.resolved = true;
    a.addr     = reinterpret_cast<uintptr_t>(sym);

    SafeMemory::ScopedSigSegvGuard guard;
    uintptr_t hit = 0;
    bool completed = guard.Try([&] {
        hit = ValidateGObjectsCandidate(a.addr);
    });

    if (!completed) {
        a.faulted = true;
        return a;
    }
    if (hit == 0) {
        // Re-run validation with detailed reporting so we know exactly which
        // step rejected the candidate.
        UE::ObjectArray::ProbeReport(a.addr, a.probeLines);
        return a;
    }
    a.validated = true;
    a.addr      = hit;     // validated address (may equal original or original wrapper)
    return a;
}

// =============================================================================
// String-ref strategy
// =============================================================================

constexpr const char* kCandidateLiterals[] = {
    // Direct global / type names — sometimes survive stripping as debug residue.
    "GUObjectArray",
    "FUObjectArray",
    "GObjectArray",
    "GObjects",
    "MaxObjectsNotConsideredByGC",
    "ObjFirstGCIndex",
    "FUObjectAllocator",
    // Function-context anchors — strings that live inside functions known to
    // load GUObjectArray (asserts, log lines, error messages). When the
    // literal-as-name is dead, these tend to still be referenced.
    "NotifyUObjectCreated",
    "NotifyUObjectDeleted",
    "FUObjectArray::AllocateUObjectIndex",
    "FUObjectArray::AllocateSerialNumber",
    "FUObjectArray::Init",
    "StaticAllocateObject",
    "UObjectBaseInit",
    "ProcessEvent",
    // Hardened UObject anchors — even heavily-stripped builds keep these
    // because they're embedded in the UObjectBase validity check methods,
    // which are inlined into virtually every UObject access path and are
    // hot enough that compilers rarely drop them.
    "UObjectBase::IsValidLowLevelFast",
    "UObjectBase::IsValidLowLevelForDestruction",
    "UObjectBase::IsValidLowLevel",
    "UObject::IsValidLowLevelFast",
    // Console var names — registered at startup, the registration callsite
    // typically references GUObjectArray's allocator counts.
    "gc.MaxObjectsNotConsideredByGC",
    "gc.SizeOfPermanentObjectPool",
};

// Scan read-only segments for the nul-terminated `needle`. Returns absolute
// VA of the first match, or 0.
INTERNAL uintptr_t FindLiteral(const std::vector<Segment>& roSegs,
                               const char* needle) noexcept {
    const size_t len = std::strlen(needle) + 1;
    for (const auto& seg : roSegs) {
        if (seg.end <= seg.start || (seg.end - seg.start) < len) continue;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(seg.start);
        const uint8_t* end  = reinterpret_cast<const uint8_t*>(seg.end) - len;
        const void*    hit  = memmem(base, end - base + 1, needle, len);
        if (hit) return reinterpret_cast<uintptr_t>(hit);
    }
    return 0;
}

INTERNAL std::vector<uint8_t> MakeUtf16LePattern(const char* needle) {
    std::vector<uint8_t> out;
    if (!needle) return out;
    const size_t len = std::strlen(needle);
    out.reserve((len + 1) * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(static_cast<uint8_t>(needle[i]));
        out.push_back(0);
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

INTERNAL void FindAllLiteralBytes(const std::vector<Segment>& segs,
                                  const void* needle,
                                  size_t len,
                                  std::vector<uintptr_t>& hits,
                                  size_t maxHits) noexcept {
    if (!needle || len == 0 || hits.size() >= maxHits) return;
    for (const auto& seg : segs) {
        if (seg.end <= seg.start || (seg.end - seg.start) < len) continue;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(seg.start);
        const uint8_t* end  = reinterpret_cast<const uint8_t*>(seg.end);
        const uint8_t* cur  = base;
        while (cur + len <= end) {
            const void* match = memmem(cur, end - cur, needle, len);
            if (!match) break;
            hits.push_back(reinterpret_cast<uintptr_t>(match));
            if (hits.size() >= maxHits) return;
            cur = reinterpret_cast<const uint8_t*>(match) + 1;
        }
    }
}

// All occurrences of `needle` (narrow, with trailing NUL) across the segment
// list. Capped at `maxHits` to bound the work in pathological cases (e.g.
// string fragments that appear hundreds of times in debug info). Many builds
// have duplicate copies of UE strings due to LTO/inlining; if the first copy is
// dead debug residue, a later copy is the one code actually references.
INTERNAL std::vector<uintptr_t> FindAllLiterals(const std::vector<Segment>& segs,
                                                const char* needle,
                                                size_t maxHits = 32) noexcept {
    std::vector<uintptr_t> hits;
    const size_t narrowLen = std::strlen(needle) + 1;
    FindAllLiteralBytes(segs, needle, narrowLen, hits, maxHits);
    return hits;
}

INTERNAL std::vector<uintptr_t> FindAllWideLiterals(const std::vector<Segment>& segs,
                                                    const char* needle,
                                                    size_t maxHits = 16) noexcept {
    std::vector<uintptr_t> hits;
    const std::vector<uint8_t> wide = MakeUtf16LePattern(needle);
    FindAllLiteralBytes(segs, wide.data(), wide.size(), hits, maxHits);
    return hits;
}

struct RefScanCounters {
    uint64_t insns       = 0;
    uint64_t adrp        = 0;
    uint64_t adr         = 0;
    uint64_t addImm      = 0;
    uint64_t ldrImm      = 0;
    uint64_t pairsAddOk  = 0;   // ADRP+ADD pairs that resolved to any address
    uint64_t pairsLdrOk  = 0;   // ADRP+LDR pairs that resolved
};

// Scan executable segments for code paths that materialize `targetAddr`.
// Catches three forms:
//   * ADRP Xd, page; ADD Xd, Xn, #lo12        — direct address, common
//   * ADRP Xd, page; LDR Xt, [Xn, #lo12]      — indirect via GOT-style slot
//   * ADR  Xd, label                          — short-range single insn
//
// The PC returned is the PC of the first instruction in the pair (or the
// ADR itself), so the caller's window walk starts from a useful anchor.
INTERNAL std::vector<uintptr_t> FindAdrpAddRefs(const std::vector<Segment>& xSegs,
                                                uintptr_t targetAddr,
                                                const std::vector<uintptr_t>& gotSlots,
                                                RefScanCounters* counters = nullptr) noexcept {
    std::vector<uintptr_t> refs;
    RefScanCounters local{};

    // Convert gotSlots to a lookup-friendly structure. Sorted vector + binary
    // search beats unordered_set for small counts (~dozens).
    // The caller builds this list once by scanning readable data segments
    // for pointers that equal targetAddr. Each slot is an 8-byte-aligned
    // address in .got/.data that stores the target pointer.

    auto isGotSlot = [&](uintptr_t a) -> bool {
        if (gotSlots.empty()) return false;
        return std::binary_search(gotSlots.begin(), gotSlots.end(), a);
    };

    for (const auto& seg : xSegs) {
        uintptr_t adrpTargetByReg[32] = {};
        uintptr_t adrpPcByReg    [32] = {};
        const uint32_t* p   = reinterpret_cast<const uint32_t*>(seg.start);
        const uint32_t* end = reinterpret_cast<const uint32_t*>(seg.end);
        for (; p + 1 <= end; ++p) {
            ++local.insns;
            uint32_t  ins = *p;
            uintptr_t pc  = reinterpret_cast<uintptr_t>(p);

            if (is_adrp(ins)) {
                ++local.adrp;
                uint32_t rd = reg_rd(ins);
                adrpTargetByReg[rd] = adrp_target(pc, ins);
                adrpPcByReg    [rd] = pc;
                continue;
            }
            if (is_adr(ins)) {
                ++local.adr;
                uintptr_t resolved = adr_target(pc, ins);
                if (resolved == targetAddr) {
                    refs.push_back(pc);
                    if (static_cast<int>(refs.size()) >= kMaxStringRefs) goto done;
                }
                continue;
            }
            if (is_add_imm64(ins)) {
                ++local.addImm;
                uint32_t rn = reg_rn(ins);
                if (adrpTargetByReg[rn]) {
                    ++local.pairsAddOk;
                    uintptr_t resolved = adrpTargetByReg[rn] + add_imm12(ins);
                    if (resolved == targetAddr) {
                        refs.push_back(adrpPcByReg[rn]);
                        if (static_cast<int>(refs.size()) >= kMaxStringRefs) goto done;
                    }
                }
                continue;
            }
            if (is_ldr_imm64(ins)) {
                ++local.ldrImm;
                uint32_t rn = reg_rn(ins);
                if (adrpTargetByReg[rn]) {
                    ++local.pairsLdrOk;
                    uintptr_t slot = adrpTargetByReg[rn] + ldr_imm12_scale8(ins);
                    // Direct: LDR-ing from the string itself (rare).
                    if (slot == targetAddr) {
                        refs.push_back(adrpPcByReg[rn]);
                        if (static_cast<int>(refs.size()) >= kMaxStringRefs) goto done;
                        continue;
                    }
                    // GOT-indirect via pre-scanned slot table — O(log n)
                    // vs. a per-instruction SafeRead (which was melting
                    // performance on huge libraries).
                    if (isGotSlot(slot)) {
                        refs.push_back(adrpPcByReg[rn]);
                        if (static_cast<int>(refs.size()) >= kMaxStringRefs) goto done;
                    }
                }
            }
        }
    }
done:
    if (counters) *counters = local;
    return refs;
}

// Overload for callers that don't have a pre-scanned GOT slot list.
INTERNAL std::vector<uintptr_t> FindAdrpAddRefs(const std::vector<Segment>& xSegs,
                                                uintptr_t targetAddr,
                                                RefScanCounters* counters = nullptr) noexcept {
    static const std::vector<uintptr_t> kEmpty;
    return FindAdrpAddRefs(xSegs, targetAddr, kEmpty, counters);
}

// Scan readable non-executable segments (data/rodata/got/relro) for 8-byte-
// aligned words equal to `targetAddr`. Returns a sorted vector of slot
// addresses. Used to accelerate GOT-indirect detection in FindAdrpAddRefs —
// with this precomputed, the hot instruction loop only needs a tiny binary
// search per ADRP+LDR pair instead of a SafeRead syscall worth of work.
INTERNAL std::vector<uintptr_t> CollectGotSlotsPointingTo(
        const std::vector<Segment>& dataSegs,
        uintptr_t targetAddr) noexcept {
    std::vector<uintptr_t> slots;
    if (targetAddr == 0) return slots;

    SafeMemory::ScopedSigSegvGuard guard;
    for (const auto& seg : dataSegs) {
        uintptr_t start = (seg.start + 7) & ~uintptr_t{7};
        if (start >= seg.end || (seg.end - start) < sizeof(uintptr_t)) continue;
        uintptr_t stop  = seg.end - sizeof(uintptr_t);

        guard.Try([&] {
            const uintptr_t* p   = reinterpret_cast<const uintptr_t*>(start);
            const uintptr_t* end = reinterpret_cast<const uintptr_t*>(stop);
            for (; p <= end; ++p) {
                if (*p == targetAddr) {
                    slots.push_back(reinterpret_cast<uintptr_t>(p));
                }
            }
        });
    }
    std::sort(slots.begin(), slots.end());
    return slots;
}

// From an ADRP at `startPc`, walk `instructions` ahead, decoding ADRP+ADD
// and ADRP+LDR pairs. For each pair whose resolved address falls in a
// writable segment, hand to `validate`. Returns the validated address
// (handed to Init), or 0.
INTERNAL uintptr_t ScanForRef(uintptr_t startPc, int instructions,
                              const std::vector<Segment>& wSegs,
                              const std::vector<Segment>& xSegs,
                              Validator validate) noexcept {
    if (!AddressInSegments(startPc, xSegs)) return 0;

    uintptr_t adrpTargetByReg[32] = {};
    const uint32_t* p   = reinterpret_cast<const uint32_t*>(startPc);
    const uint32_t* end = p + instructions;

    for (; p < end; ++p) {
        if (!AddressInSegments(reinterpret_cast<uintptr_t>(p), xSegs)) break;
        uint32_t  ins = *p;
        uintptr_t pc  = reinterpret_cast<uintptr_t>(p);

        if (is_adrp(ins)) {
            adrpTargetByReg[reg_rd(ins)] = adrp_target(pc, ins);
            continue;
        }
        if (is_add_imm64(ins)) {
            uint32_t rn = reg_rn(ins);
            if (adrpTargetByReg[rn]) {
                uintptr_t resolved = adrpTargetByReg[rn] + add_imm12(ins);
                if (AddressInSegments(resolved, wSegs)) {
                    uintptr_t hit = validate(resolved);
                    if (hit) return hit;
                }
            }
            continue;
        }
        if (is_ldr_imm64(ins)) {
            uint32_t rn = reg_rn(ins);
            if (adrpTargetByReg[rn]) {
                uintptr_t resolved = adrpTargetByReg[rn] + ldr_imm12_scale8(ins);
                // Case 1: the LDR reads directly from a writable-segment
                // slot — classic ADRP+LDR addressing pattern where the
                // slot itself IS the global.
                if (AddressInSegments(resolved, wSegs)) {
                    uintptr_t hit = validate(resolved);
                    if (hit) return hit;
                    // Fall through to Case 2: the slot may still be a
                    // pointer slot rather than the global itself. Try
                    // dereferencing once. We only fall through here when
                    // the slot is in-bounds, so SafeMemory::SafeRead won't
                    // fault on us — no need for a guard.
                    auto v = SafeMemory::SafeReadAny<uintptr_t>(resolved);
                    if (v && *v != 0 && AddressInSegments(*v, wSegs)) {
                        hit = validate(*v);
                        if (hit) return hit;
                    }
                }
                // else: LDR target isn't in a writable libUE4 segment
                // (probably an intra-function pool, or reference to
                // code/rodata). Don't try to deref — scrubbing arbitrary
                // addresses with SafeRead here was pinning the scanner
                // on large libraries.
            }
            continue;
        }
    }
    return 0;
}

// Same as ScanForRef but, on failure, follows every B/BL within the window
// up to `depth` levels deep into callees, scanning each callee's first
// `branchScanInstructions` instructions for an ADRP+ADD/LDR validating to
// writable. This handles chains like:
//
//      caller:                FName::FName:           FNamePool::Find:
//        ADRP X1, "Byte..."     stp x29, x30, ...      ADRP X1, NamePoolData
//        ADD  X1, X1, #lo12     ...                    ADD  X1, X1, #lo12   <-- the load
//        BL   FName::FName      BL FNamePool::Find     ...
//
// where the literal-ref site is two function-call hops from the actual
// global. depth=1 catches the one-hop case (caller → FName::FName); depth=2
// catches the two-hop case (caller → FName::FName → FNamePool::Find).
// Every candidate goes through `validate`, so deep recursion can't false-
// positive — only inflate cost.
INTERNAL uintptr_t ScanForRefWithBranchFollow(
        uintptr_t startPc, int instructions,
        int branchScanInstructions, int depth,
        const std::vector<Segment>& wSegs,
        const std::vector<Segment>& xSegs,
        Validator validate) noexcept {
    if (uintptr_t hit = ScanForRef(startPc, instructions, wSegs, xSegs, validate))
        return hit;
    if (depth <= 0) return 0;
    if (!AddressInSegments(startPc, xSegs)) return 0;

    const uint32_t* p   = reinterpret_cast<const uint32_t*>(startPc);
    const uint32_t* end = p + instructions;
    for (; p < end; ++p) {
        if (!AddressInSegments(reinterpret_cast<uintptr_t>(p), xSegs)) break;
        const uint32_t ins = *p;
        // B (0x14000000) / BL (0x94000000): top 6 bits 000101 / 100101.
        if ((ins & 0x7C000000) != 0x14000000) continue;

        int32_t imm26 = ins & 0x03FFFFFF;
        if (imm26 & 0x02000000) imm26 |= 0xFC000000;          // sign-extend
        const uintptr_t target =
                reinterpret_cast<uintptr_t>(p) +
                (static_cast<int64_t>(imm26) << 2);
        if (!AddressInSegments(target, xSegs)) continue;

        if (uintptr_t inner = ScanForRefWithBranchFollow(
                target, branchScanInstructions, branchScanInstructions,
                depth - 1, wSegs, xSegs, validate))
            return inner;
    }
    return 0;
}

// =============================================================================
// Code-ref strategy (engine-agnostic)
// -----------------------------------------------------------------------------
// Walk every executable segment instruction-by-instruction. Maintain a
// per-register ADRP-target map. Whenever an ADD/LDR completes a pair whose
// resolved address is in a writable segment, run ObjectArray::ProbeNoGuard
// on it. The first probe that passes is GUObjectArray.
//
// This subsumes the old "find specific function via its strings then
// disassemble" approach because every code reference to GUObjectArray —
// from any function, in any engine version — is a candidate. False
// positives are filtered by Probe's vtable-in-libUE4 + InternalIndex==i
// invariants, which are tight enough that the very first match is reliable.
// =============================================================================

INTERNAL bool CodeRefQuickScan(const UE4Info& info, Validator validate,
                               const char* targetLabel, Result& out) noexcept {
    auto wSegs = CollectSegments(info.base, /*req*/ PF_W,           /*forbid*/ 0);
    auto xSegs = CollectSegments(info.base, /*req*/ PF_R | PF_X,    /*forbid*/ PF_W);
    if (wSegs.empty() || xSegs.empty()) {
        out.detail = "missing writable / executable segments";
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;

    constexpr int kForwardWindow = 16;
    constexpr uintptr_t kMaxQuickDelta = 0x20000;

    uint32_t scanned = 0;
    uintptr_t hit = 0;
    uint64_t insnsScanned = 0;
    uint64_t totalBytes = 0;
    for (const auto& xseg : xSegs) {
        totalBytes += (SegmentBytes(xseg) / sizeof(uint32_t)) * sizeof(uint32_t);
    }

    uint64_t completedBytes = 0;
    uint64_t scannedBytes = 0;
    uint64_t segmentBytes = 0;
    uint64_t segmentScannedBytes = 0;
    uint32_t segmentIndex = 0;
    const uint32_t segmentCount = static_cast<uint32_t>(xSegs.size());

    auto logProgress = [&](const char* tag) {
        const double pct = totalBytes > 0
                         ? (100.0 * static_cast<double>(scannedBytes) /
                            static_cast<double>(totalBytes))
                         : 0.0;
        UpdateCodeRefProgress(targetLabel, tag,
                              segmentIndex, segmentCount,
                              totalBytes, scannedBytes,
                              segmentBytes, segmentScannedBytes,
                              scanned);
        DLOGI("[coderef-quick:%s] %s %.1f%% total=%llu/%llu bytes chunk=%u/%u %llu/%llu bytes insns=%llu scanned-loads=%u",
              targetLabel, tag, pct,
              (unsigned long long)scannedBytes,
              (unsigned long long)totalBytes,
              segmentIndex, segmentCount,
              (unsigned long long)segmentScannedBytes,
              (unsigned long long)segmentBytes,
              (unsigned long long)insnsScanned,
              scanned);
    };
    logProgress("start");

    uint64_t nextHeartbeat = 1u << 22;

    for (const auto& xseg : xSegs) {
        if (hit) break;
        ++segmentIndex;
        segmentBytes = (SegmentBytes(xseg) / sizeof(uint32_t)) * sizeof(uint32_t);
        segmentScannedBytes = 0;
        scannedBytes = completedBytes;
        logProgress("chunk");

        const uint32_t* p = reinterpret_cast<const uint32_t*>(xseg.start);
        const uint32_t* end = reinterpret_cast<const uint32_t*>(xseg.end);

        for (; p < end && !hit; ++p) {
            const uint64_t rawSegmentBytes =
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p + 1) - xseg.start);
            segmentScannedBytes = std::min(rawSegmentBytes, segmentBytes);
            scannedBytes = completedBytes + segmentScannedBytes;

            if ((++insnsScanned) >= nextHeartbeat) {
                logProgress("progress");
                nextHeartbeat += (1u << 22);
            }

            const uint32_t ins = *p;
            if (!is_adrp(ins)) continue;

            const uintptr_t pc = reinterpret_cast<uintptr_t>(p);
            const uintptr_t base = adrp_target(pc, ins);
            if (!PageCanReachSegments(base, wSegs, kMaxQuickDelta)) continue;

            const uint32_t baseReg = reg_rd(ins);
            const uint32_t* q = p + 1;
            const ptrdiff_t remaining = end > q ? end - q : 0;
            const ptrdiff_t window = std::min<ptrdiff_t>(remaining, kForwardWindow);
            const uint32_t* qEnd = q + window;
            for (; q < qEnd && !hit; ++q) {
                const uint32_t next = *q;
                if (is_adrp(next) && reg_rd(next) == baseReg) break;

                uintptr_t resolved = 0;
                if (is_add_imm64(next) && reg_rn(next) == baseReg) {
                    resolved = base + add_imm12(next);
                } else if (is_ldr_imm64(next) && reg_rn(next) == baseReg) {
                    resolved = base + ldr_imm12_scale8(next);
                } else {
                    continue;
                }

                if (!AddressInSegments(resolved, wSegs)) continue;
                ++scanned;

                uintptr_t v = 0;
                guard.Try([&] { v = validate(resolved); });
                if (v) {
                    hit = v;
                    break;
                }

                if (is_ldr_imm64(next)) {
                    uintptr_t storedPtr = 0;
                    guard.Try([&] {
                        auto rd = SafeMemory::SafeReadAny<uintptr_t>(resolved);
                        if (rd) storedPtr = *rd;
                    });
                    if (storedPtr != 0 && AddressInSegments(storedPtr, wSegs)) {
                        ++scanned;
                        guard.Try([&] { v = validate(storedPtr); });
                        if (v) {
                            hit = v;
                            break;
                        }
                    }
                }
            }
        }

        if (!hit) {
            segmentScannedBytes = segmentBytes;
            completedBytes += segmentBytes;
            scannedBytes = completedBytes;
            logProgress("chunk-done");
        }
    }

    logProgress(hit ? "hit" : "done");
    FinishCodeRefProgress(targetLabel, hit ? "quick-hit" : "quick-done",
                          totalBytes, scannedBytes, scanned);

    out.candidatesScanned = scanned;
    if (!hit) {
        out.detail = "code-ref-quick: scanned " + std::to_string(scanned) +
                     " nearby writable refs, none validated";
        return false;
    }

    out.ok = true;
    out.method = Method::CodeRefQuick;
    out.address = hit;
    out.offset = hit - info.base;

    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "code-ref-quick [%s] matched at 0x%lx (offset 0x%lx, %u nearby refs probed)",
                  targetLabel,
                  static_cast<unsigned long>(hit),
                  static_cast<unsigned long>(out.offset),
                  scanned);
    out.detail = buf;
    return true;
}

INTERNAL bool CodeRefScan(const UE4Info& info, Validator validate,
                          const char* targetLabel, Result& out) noexcept {
    auto wSegs = CollectSegments(info.base, /*req*/ PF_W,           /*forbid*/ 0);
    auto xSegs = CollectSegments(info.base, /*req*/ PF_R | PF_X,    /*forbid*/ PF_W);
    if (wSegs.empty() || xSegs.empty()) {
        out.detail = "missing writable / executable segments";
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;

    uint32_t  scanned = 0;
    uintptr_t hit     = 0;
    uint64_t  insnsScanned = 0;
    uint64_t  totalBytes = 0;
    for (const auto& xseg : xSegs) {
        totalBytes += (SegmentBytes(xseg) / sizeof(uint32_t)) * sizeof(uint32_t);
    }

    uint64_t  completedBytes = 0;
    uint64_t  scannedBytes = 0;
    uint64_t  segmentBytes = 0;
    uint64_t  segmentScannedBytes = 0;
    uint32_t  segmentIndex = 0;
    const uint32_t segmentCount = static_cast<uint32_t>(xSegs.size());

    // Heartbeat: on large libraries (hundreds of MB .text) this loop runs
    // tens of millions of iterations, and the per-candidate validator is
    // expensive. Without progress output the operator can't tell a slow
    // scan apart from a hang.
    auto logProgress = [&](const char* tag) {
        const double pct = totalBytes > 0
                         ? (100.0 * static_cast<double>(scannedBytes) /
                            static_cast<double>(totalBytes))
                         : 0.0;
        UpdateCodeRefProgress(targetLabel, tag,
                              segmentIndex, segmentCount,
                              totalBytes, scannedBytes,
                              segmentBytes, segmentScannedBytes,
                              scanned);
        DLOGI("[coderef:%s] %s %.1f%% total=%llu/%llu bytes chunk=%u/%u %llu/%llu bytes insns=%llu scanned-loads=%u",
              targetLabel, tag, pct,
              (unsigned long long)scannedBytes,
              (unsigned long long)totalBytes,
              segmentIndex, segmentCount,
              (unsigned long long)segmentScannedBytes,
              (unsigned long long)segmentBytes,
              (unsigned long long)insnsScanned,
              scanned);
    };
    logProgress("start");

    uint64_t nextHeartbeat = 1u << 22;  // roughly every 4M instructions

    for (const auto& xseg : xSegs) {
        if (hit) break;
        ++segmentIndex;
        segmentBytes = (SegmentBytes(xseg) / sizeof(uint32_t)) * sizeof(uint32_t);
        segmentScannedBytes = 0;
        scannedBytes = completedBytes;
        logProgress("chunk");

        uintptr_t adrpTargetByReg[32] = {};
        const uint32_t* p   = reinterpret_cast<const uint32_t*>(xseg.start);
        const uint32_t* end = reinterpret_cast<const uint32_t*>(xseg.end);

        for (; p < end && !hit; ++p) {
            const uint64_t rawSegmentBytes =
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p + 1) - xseg.start);
            segmentScannedBytes = std::min(rawSegmentBytes, segmentBytes);
            scannedBytes = completedBytes + segmentScannedBytes;

            if ((++insnsScanned) >= nextHeartbeat) {
                logProgress("progress");
                nextHeartbeat += (1u << 22);
            }
            uint32_t  ins = *p;
            uintptr_t pc  = reinterpret_cast<uintptr_t>(p);

            if (is_adrp(ins)) {
                adrpTargetByReg[reg_rd(ins)] = adrp_target(pc, ins);
                continue;
            }
            if (is_add_imm64(ins)) {
                uint32_t rn = reg_rn(ins);
                uintptr_t base = adrpTargetByReg[rn];
                if (!base) continue;
                uintptr_t resolved = base + add_imm12(ins);
                if (!AddressInSegments(resolved, wSegs)) continue;
                ++scanned;
                uintptr_t v = 0;
                guard.Try([&] { v = validate(resolved); });
                if (v) { hit = v; break; }
                continue;
            }
            if (is_ldr_imm64(ins)) {
                uint32_t rn = reg_rn(ins);
                uintptr_t base = adrpTargetByReg[rn];
                if (!base) continue;
                uintptr_t resolved = base + ldr_imm12_scale8(ins);
                if (!AddressInSegments(resolved, wSegs)) continue;
                ++scanned;
                // Try the slot itself first (stock layout).
                uintptr_t v = 0;
                guard.Try([&] { v = validate(resolved); });
                if (v) { hit = v; break; }
                // GOT-indirect fallback: LDR reads a pointer from the slot.
                // Dereference once; if the stored pointer is also in a
                // writable segment, try validating it.
                uintptr_t storedPtr = 0;
                guard.Try([&] {
                    auto rd = SafeMemory::SafeReadAny<uintptr_t>(resolved);
                    if (rd) storedPtr = *rd;
                });
                if (storedPtr != 0 && AddressInSegments(storedPtr, wSegs)) {
                    ++scanned;
                    guard.Try([&] { v = validate(storedPtr); });
                    if (v) { hit = v; break; }
                }
            }
        }

        if (!hit) {
            segmentScannedBytes = segmentBytes;
            completedBytes += segmentBytes;
            scannedBytes = completedBytes;
            logProgress("chunk-done");
        }
    }

    logProgress(hit ? "hit" : "done");
    FinishCodeRefProgress(targetLabel, hit ? "hit" : "done",
                          totalBytes, scannedBytes, scanned);

    out.candidatesScanned = scanned;
    if (!hit) {
        out.detail = "code-ref: scanned " + std::to_string(scanned) +
                     " writable-segment loads, none validated";
        return false;
    }

    out.ok      = true;
    out.method  = Method::CodeRef;
    out.address = hit;
    out.offset  = hit - info.base;

    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "code-ref [%s] matched at 0x%lx (offset 0x%lx, %u writable refs probed)",
                  targetLabel,
                  static_cast<unsigned long>(hit),
                  static_cast<unsigned long>(out.offset), scanned);
    out.detail = buf;
    return true;
}

// =============================================================================
// Pattern strategy
// -----------------------------------------------------------------------------
// User supplies an IDA-style byte pattern (e.g. an instruction prologue
// known to be in a function that loads GUObjectArray). The pattern is
// matched against executable segments. For each hit, walk forward decoding
// ADRP+ADD/LDR pairs and validate writable targets — same machinery as
// CodeRef, but constrained to the matched window.
// =============================================================================

constexpr int kPatternForwardInstructions = 256;

INTERNAL bool PatternScan(const UE4Info& info, const char* sig,
                          Validator validate, const char* targetLabel,
                          Result& out) noexcept {
    if (sig == nullptr || *sig == 0) {
        out.detail = "pattern: empty pattern";
        return false;
    }

    PatternScanner::Pattern pat = PatternScanner::Parse(sig);
    if (pat.empty()) {
        out.detail = "pattern: parse failed (use \"AA BB ?? CC\" form)";
        return false;
    }

    auto xSegs = CollectSegments(info.base, /*req*/ PF_R | PF_X, /*forbid*/ PF_W);
    auto wSegs = CollectSegments(info.base, /*req*/ PF_W,        /*forbid*/ 0);
    if (xSegs.empty() || wSegs.empty()) {
        out.detail = "pattern: missing executable / writable segments";
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;
    uint32_t totalHits = 0;

    for (const auto& xseg : xSegs) {
        auto hits = PatternScanner::ScanAll(xseg.start, xseg.end - xseg.start, pat,
                                            /*maxHits*/ 256);
        totalHits += static_cast<uint32_t>(hits.size());
        for (uintptr_t hitPc : hits) {
            uintptr_t v = 0;
            guard.Try([&] {
                v = ScanForRef(hitPc, kPatternForwardInstructions, wSegs, xSegs, validate);
            });
            if (v) {
                out.ok      = true;
                out.method  = Method::Pattern;
                out.address = v;
                out.offset  = v - info.base;
                out.candidatesScanned = totalHits;

                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "pattern matched at 0x%lx (%u total hits) -> %s at 0x%lx (offset 0x%lx)",
                              static_cast<unsigned long>(hitPc), totalHits,
                              targetLabel,
                              static_cast<unsigned long>(v),
                              static_cast<unsigned long>(out.offset));
                out.detail = buf;
                return true;
            }
        }
    }

    out.candidatesScanned = totalHits;
    if (totalHits == 0) {
        out.detail = "pattern: no matches in any executable segment";
    } else {
        out.detail = std::string("pattern: ") + std::to_string(totalHits) +
                     " hits, none led to a valid " + targetLabel + " within window";
    }
    return false;
}

// =============================================================================
// BSS scan strategy
// =============================================================================

INTERNAL bool BssScan(const UE4Info& info, Validator validate,
                      const char* targetLabel, Result& out) noexcept {
    auto wSegs = CollectSegments(info.base, /*req*/ PF_W, /*forbid*/ 0);
    if (wSegs.empty()) {
        out.detail = "no writable segments found";
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;

    uint32_t  scanned = 0;
    uintptr_t hit     = 0;

    for (const auto& seg : wSegs) {
        if (seg.end < seg.start + kMinSegmentTailBytes) continue;
        const uintptr_t stop = seg.end - kMinSegmentTailBytes;
        for (uintptr_t addr = seg.start; addr <= stop; addr += kScanStride) {
            ++scanned;
            uintptr_t v = 0;
            guard.Try([&] { v = validate(addr); });
            if (v) { hit = v; break; }
        }
        if (hit) break;
    }

    out.candidatesScanned = scanned;
    if (!hit) {
        out.detail = "scanned " + std::to_string(scanned) +
                     " candidates, no validating layout found";
        return false;
    }

    out.ok      = true;
    out.method  = Method::BssScan;
    out.address = hit;
    out.offset  = hit - info.base;

    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "BSS scan [%s] matched at 0x%lx (offset 0x%lx, %u candidates)",
                  targetLabel,
                  static_cast<unsigned long>(hit),
                  static_cast<unsigned long>(out.offset), scanned);
    out.detail = buf;
    return true;
}

// =============================================================================
// Deep structural scan — every writable segment, every loaded library
// -----------------------------------------------------------------------------
// Symbol/string/code/BSS strategies all assume libUE4 hosts GUObjectArray.
// On modular UE5 builds the global may live in a sibling .so. DeepScan walks
// writable segments across every loaded module and validates each candidate
// with the same Probe used by BssScan — vtable check now spans all loaded
// executable segments (see SafeMemory::IsExecutable), so vtables hosted in
// any UE module satisfy the invariant.
// =============================================================================

INTERNAL bool DeepScan(const UE4Info& info, Result& out) noexcept {
    auto wSegs = CollectSegmentsAllLibs(/*req*/ PF_W, /*forbid*/ 0);
    if (wSegs.empty()) {
        out.detail = "deep-scan: no writable segments found across loaded libraries";
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;

    uint32_t  scanned = 0;
    uintptr_t hit     = 0;

    for (const auto& seg : wSegs) {
        if (seg.end < seg.start + kMinSegmentTailBytes) continue;
        const uintptr_t stop = seg.end - kMinSegmentTailBytes;
        for (uintptr_t addr = seg.start; addr <= stop; addr += kScanStride) {
            ++scanned;
            uintptr_t v = 0;
            guard.Try([&] { v = ValidateGObjectsCandidate(addr); });
            if (v) { hit = v; break; }
        }
        if (hit) break;
    }

    out.candidatesScanned = scanned;
    if (!hit) {
        out.detail = "deep-scan: " + std::to_string(scanned) +
                     " candidates across " + std::to_string(wSegs.size()) +
                     " writable segments, none validated";
        return false;
    }

    out.ok      = true;
    out.method  = Method::DeepScan;
    out.address = hit;
    out.offset  = (info.base != 0 && hit >= info.base) ? (hit - info.base) : 0;

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "deep-scan matched at 0x%lx (rel libUE4 0x%lx, %u candidates, %zu segments)",
                  static_cast<unsigned long>(hit),
                  static_cast<unsigned long>(out.offset),
                  scanned, wSegs.size());
    out.detail = buf;
    return true;
}

// =============================================================================
// String-ref entrypoint
// =============================================================================

INTERNAL bool StringRefScan(const UE4Info& info,
                            const char* const* literals, size_t literalCount,
                            Validator validate, const char* targetLabel,
                            Result& out,
                            bool wideLiterals = false) noexcept {
    auto trace = [&](const char* fmt, ...) {
        char b[224];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        out.traceLines.emplace_back(b);
        DLOGI("[strref:%s] %s", targetLabel, b);
    };

    // Broadened: rodata may be merged into the R+X segment on some toolchains,
    // or live in RELRO. Search every readable libUE4 PT_LOAD for literals.
    auto allReadable = CollectSegments(info.base, /*req*/ PF_R, /*forbid*/ 0);
    auto xSegs       = CollectSegments(info.base, /*req*/ PF_R | PF_X, /*forbid*/ PF_W);
    auto wSegs       = CollectSegments(info.base, /*req*/ PF_W, /*forbid*/ 0);
    // Non-executable readable segments hold .got / .data.rel.ro / .rodata —
    // places where a pointer to our literal might live so that ADRP+LDR
    // pairs can materialize it indirectly. We pre-scan these once per
    // literal and hand the result to FindAdrpAddRefs.
    auto dataSegs    = CollectSegments(info.base, /*req*/ PF_R, /*forbid*/ PF_X);

    trace("segments: readable=%zu exec=%zu writable=%zu encoding=%s",
          allReadable.size(), xSegs.size(), wSegs.size(),
          wideLiterals ? "utf16le" : "narrow");

    if (allReadable.empty() || xSegs.empty() || wSegs.empty()) {
        out.detail = "string-ref: missing readable / text / data segments";
        return false;
    }

    char buf[256];
    SafeMemory::ScopedSigSegvGuard guard;
    uint32_t totalRefs = 0;
    bool     anyLiteralFound = false;

    for (size_t li = 0; li < literalCount; ++li) {
        const char* literal = literals[li];
        auto strAddrs = wideLiterals
                ? FindAllWideLiterals(allReadable, literal)
                : FindAllLiterals(allReadable, literal);
        if (strAddrs.empty()) {
            trace("[\"%s\"] not in readable segments", literal);
            continue;
        }
        anyLiteralFound = true;
        trace("[\"%s\"] %zu occurrence%s in readable segments",
              literal, strAddrs.size(), strAddrs.size() == 1 ? "" : "s");

        for (size_t occ = 0; occ < strAddrs.size(); ++occ) {
            uintptr_t strAddr = strAddrs[occ];

            // Build the GOT-indirect slot list once per string occurrence.
            auto gotSlots = CollectGotSlotsPointingTo(dataSegs, strAddr);

            RefScanCounters cnt{};
            auto refs = FindAdrpAddRefs(xSegs, strAddr, gotSlots, &cnt);
            trace("  occ[%zu] @ 0x%lx -> %zu refs (pairs ADRP+ADD=%llu LDR=%llu, ADR=%llu, gotSlots=%zu)",
                  occ, (unsigned long)strAddr, refs.size(),
                  (unsigned long long)cnt.pairsAddOk,
                  (unsigned long long)cnt.pairsLdrOk,
                  (unsigned long long)cnt.adr,
                  gotSlots.size());
            totalRefs += static_cast<uint32_t>(refs.size());
            if (refs.empty()) continue;

            for (size_t i = 0; i < refs.size(); ++i) {
                uintptr_t refPc = refs[i];
                uintptr_t hit = 0;

                guard.Try([&] {
                    hit = ScanForRefWithBranchFollow(refPc, kStringRefForward,
                                                     /*branchScanInsns*/ 96,
                                                     /*depth*/ 2,
                                                     wSegs, xSegs, validate);
                });
                if (hit) {
                    std::snprintf(buf, sizeof(buf),
                                  "string-ref [%s] via \"%s\" occ[%zu] ref[%zu]@0x%lx -> match at 0x%lx (offset 0x%lx)",
                                  targetLabel, literal, occ, i, (unsigned long)refPc,
                                  (unsigned long)hit, (unsigned long)(hit - info.base));
                    out.ok      = true;
                    out.method  = Method::StringRef;
                    out.address = hit;
                    out.offset  = hit - info.base;
                    out.detail  = buf;
                    trace("    ref[%zu] @ 0x%lx FORWARD WIN -> 0x%lx VALIDATED",
                          i, (unsigned long)refPc, (unsigned long)hit);
                    return true;
                }

                uintptr_t back = refPc - static_cast<uintptr_t>(kStringRefBackward) * 4;
                guard.Try([&] {
                    hit = ScanForRefWithBranchFollow(
                            back, kStringRefBackward + kStringRefForward,
                            /*branchScanInsns*/ 96, /*depth*/ 2,
                            wSegs, xSegs, validate);
                });
                if (hit) {
                    std::snprintf(buf, sizeof(buf),
                                  "string-ref [%s] via \"%s\" occ[%zu] ref[%zu]@0x%lx (back) -> match at 0x%lx (offset 0x%lx)",
                                  targetLabel, literal, occ, i, (unsigned long)refPc,
                                  (unsigned long)hit, (unsigned long)(hit - info.base));
                    out.ok      = true;
                    out.method  = Method::StringRef;
                    out.address = hit;
                    out.offset  = hit - info.base;
                    out.detail  = buf;
                    trace("    ref[%zu] @ 0x%lx BACK+FORWARD WIN -> 0x%lx VALIDATED",
                          i, (unsigned long)refPc, (unsigned long)hit);
                    return true;
                }
            }
        }
    }

    out.candidatesScanned = totalRefs;
    if (!anyLiteralFound) {
        std::snprintf(buf, sizeof(buf),
                      "string-ref [%s]: none of %zu known literals were present",
                      targetLabel, literalCount);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "string-ref [%s]: %u refs across %zu literals, none led to a valid target",
                      targetLabel, totalRefs, literalCount);
    }
    out.detail = buf;
    return false;
}

// =============================================================================
// Common timing wrapper
// =============================================================================

template <class Fn>
INTERNAL Result TimedRun(Fn&& fn) noexcept {
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    fn(r);
    auto t1 = std::chrono::steady_clock::now();
    r.elapsedMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

Result FindGObjects_Symbol(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);

        const size_t n = sizeof(kGObjectsSymbols) / sizeof(kGObjectsSymbols[0]);
        DLOGI("FindGObjects_Symbol: trying %zu candidate names", n);

        for (const auto& sn : kGObjectsSymbols) {
            SymbolAttempt a = TrySymbolDetailed(info, sn.name);

            char head[160];
            if (!a.resolved) {
                std::snprintf(head, sizeof(head),
                              "[%s] not in symtab", sn.name);
                r.traceLines.emplace_back(head);
                continue;
            }
            if (a.faulted) {
                std::snprintf(head, sizeof(head),
                              "[%s @ 0x%lx %s] FAULTED during validation",
                              sn.name, (unsigned long)a.addr,
                              a.fromDisk ? "(disk)" : "(dynsym)");
                r.traceLines.emplace_back(head);
                DLOGW("OffsetFinder: %s", head);
                continue;
            }
            if (!a.validated) {
                std::snprintf(head, sizeof(head),
                              "[%s @ 0x%lx %s] resolved but Probe rejected:",
                              sn.name, (unsigned long)a.addr,
                              a.fromDisk ? "(disk)" : "(dynsym)");
                r.traceLines.emplace_back(head);
                for (const auto& line : a.probeLines) {
                    r.traceLines.emplace_back(std::string("    ") + line);
                }
                DLOGW("OffsetFinder: %s", head);
                continue;
            }

            // Success.
            r.ok      = true;
            r.method  = Method::Symbol;
            r.address = a.addr;
            r.offset  = a.addr - info.base;
            r.detail  = std::string("symbol: ") + sn.name;

            std::snprintf(head, sizeof(head),
                          "[%s @ 0x%lx %s] VALIDATED -> offset 0x%lx",
                          sn.name, (unsigned long)a.addr,
                          a.fromDisk ? "(disk)" : "(dynsym)",
                          (unsigned long)r.offset);
            r.traceLines.emplace_back(head);
            DLOGI("OffsetFinder: %s", head);
            return;
        }
        r.detail = "all symbol attempts exhausted (see trace)";
    });
}

Result FindGObjects_StringRef(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        StringRefScan(info, kCandidateLiterals,
                      sizeof(kCandidateLiterals) / sizeof(kCandidateLiterals[0]),
                      &ValidateGObjectsCandidate, "GUObjectArray", r);
    });
}

Result FindGObjects_CodeRef(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        CodeRefScan(info, &ValidateGObjectsCandidate, "GUObjectArray", r);
    });
}

Result FindGObjects_CodeRefQuick(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        CodeRefQuickScan(info, &ValidateGObjectsCandidate, "GUObjectArray", r);
    });
}

Result FindGObjects_Pattern(const UE4Info& info, const char* patternSig) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        PatternScan(info, patternSig, &ValidateGObjectsCandidate, "GUObjectArray", r);
    });
}

Result FindGObjects_BssScan(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        BssScan(info, &ValidateGObjectsCandidate, "GUObjectArray", r);
    });
}

Result FindGObjects_DeepScan(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        // DeepScan tolerates a missing libUE4 base — it walks every loaded
        // module — but we still need libUE4 for the offset to be meaningful.
        EnsureInitialized(info);
        DeepScan(info, r);
    });
}

namespace {

INTERNAL std::string LookupLibName(uintptr_t addr) noexcept {
    struct Ctx { uintptr_t addr; std::string out; };
    Ctx ctx{ addr, {} };
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& p = info->dlpi_phdr[i];
            if (p.p_type != PT_LOAD) continue;
            uintptr_t s = info->dlpi_addr + p.p_vaddr;
            uintptr_t e = s + p.p_memsz;
            if (c->addr >= s && c->addr < e) {
                c->out = info->dlpi_name ? info->dlpi_name : "(main)";
                return 1;
            }
        }
        return 0;
    }, &ctx);
    return ctx.out;
}

INTERNAL bool LooseLooksChunked(uintptr_t base, LooseCandidate& out) noexcept {
    auto numEl  = SafeMemory::SafeReadAny<int32_t>  (base + Off::FChunkedFixedUObjectArray::NumElements);
    auto maxEl  = SafeMemory::SafeReadAny<int32_t>  (base + Off::FChunkedFixedUObjectArray::MaxElements);
    auto chunks = SafeMemory::SafeReadAny<uintptr_t>(base + Off::FChunkedFixedUObjectArray::Objects);
    if (!numEl || !maxEl || !chunks) return false;
    if (*numEl <= 64 || *numEl > 100'000'000) return false;
    if (*maxEl < *numEl) return false;
    if (*chunks == 0) return false;
    out.address     = base;
    out.chunked     = true;
    out.numElements = *numEl;
    out.maxElements = *maxEl;
    out.objectsPtr  = *chunks;
    return true;
}

INTERNAL void LoosePopulateFirstObject(LooseCandidate& c) noexcept {
    if (c.objectsPtr == 0) return;
    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        auto firstChunk = SafeMemory::SafeReadAny<uintptr_t>(c.objectsPtr);
        if (!firstChunk || *firstChunk == 0) return;
        c.firstChunk = *firstChunk;
        auto rawObj = SafeMemory::SafeReadAny<uintptr_t>(*firstChunk);
        if (!rawObj || *rawObj == 0) return;
        c.firstObject = *rawObj;

        auto vt = SafeMemory::SafeReadAny<uintptr_t>(c.firstObject);
        if (vt) {
            c.firstVtable = *vt;
            c.vtableExec  = SafeMemory::IsExecutable(*vt);
        }
        auto idx = SafeMemory::SafeReadAny<int32_t>(c.firstObject + Off::UObject::InternalIndex);
        if (idx) {
            c.firstObjIdx = *idx;
            c.firstObjIdxReadable = true;
        }
        // Hex dump the first 64 bytes — fast structural inspection.
        for (size_t i = 0; i < sizeof(c.objHex); ++i) {
            auto b = SafeMemory::SafeReadAny<uint8_t>(c.firstObject + i);
            if (!b) return;
            c.objHex[i] = *b;
        }
        c.objHexReadable = true;
    });
}

}  // namespace

std::vector<LooseCandidate> FindGObjects_Loose(const UE4Info& info,
                                               bool allLibraries,
                                               size_t maxResults) noexcept {
    std::vector<LooseCandidate> hits;
    if (info.base == 0 && !allLibraries) return hits;
    EnsureInitialized(info);

    auto wSegs = allLibraries
            ? CollectSegmentsAllLibs(/*req*/ PF_W, /*forbid*/ 0)
            : CollectSegments       (info.base, /*req*/ PF_W, /*forbid*/ 0);
    if (wSegs.empty()) return hits;

    SafeMemory::ScopedSigSegvGuard guard;

    for (const auto& seg : wSegs) {
        if (seg.end < seg.start + kMinSegmentTailBytes) continue;
        const uintptr_t stop = seg.end - kMinSegmentTailBytes;
        for (uintptr_t addr = seg.start; addr <= stop && hits.size() < maxResults;
             addr += kScanStride) {
            LooseCandidate c;
            bool ok = false;
            guard.Try([&] { ok = LooseLooksChunked(addr, c); });
            if (!ok) continue;

            LoosePopulateFirstObject(c);
            c.libraryName = LookupLibName(addr);
            hits.push_back(std::move(c));
        }
        if (hits.size() >= maxResults) break;
    }
    return hits;
}

// Auto chain: Symbol -> String-ref -> Code-ref-quick -> Code-ref -> BSS.
// per user request; Pattern stays out of the chain because it needs a
// caller-supplied sig.
Result FindGObjects(const UE4Info& info) noexcept {
    Result r = FindGObjects_Symbol(info);
    if (r.ok) return r;
    r = FindGObjects_StringRef(info);
    if (r.ok) return r;
    r = FindGObjects_CodeRefQuick(info);
    if (r.ok) return r;
    r = FindGObjects_CodeRef(info);
    if (r.ok) return r;
    r = FindGObjects_BssScan(info);
    if (r.ok) return r;
    return FindGObjects_DeepScan(info);
}

// =============================================================================
// GNames / NamePoolData finder — symbol pass + substring scan over .dynsym.
//
// Hardcoded mangled names like _ZN9FNamePool12NamePoolDataE are fragile
// because the digit prefixes encode the length of each name component. Any
// namespace wrapper or rename changes the mangled string entirely. To cope,
// after the exact-name attempts we walk the entire .dynsym table looking
// for any symbol whose name *contains* "NamePool" / "GNames" / "GName"
// distinctive substrings. Every match is Probe-validated, so a false
// substring hit is harmless.
// =============================================================================

namespace {

constexpr SymbolName kGNamesSymbols[] = {
    { "NamePoolData",                  "UE4.23+ FNamePool global"     },
    { "GNames",                        "older TNameEntryArray"        },
    { "GName",                         "very old engine"              },
    { "GNameBlocksDebug",              "UE5 debug-visualisation alias"},
    { "FName_NamePoolData",            "alt naming"                   },
    { "GNames_NamePoolData",           "alt naming"                   },
    { "MT_NamePoolData",               "memory-tracker prefix"        },
    { "MT_GNames",                     "memory-tracker prefix"        },
    { "OldNames",                      "legacy table"                 },
    { "FName::Names",                  "static accessor (rare)"       },
    { "_ZN9FNamePool12NamePoolDataE",  "FNamePool::NamePoolData mangled" },
    { "_ZN5FName10NamePoolDataE",      "FName::NamePoolData mangled"  },
    { "_ZN5FName6GNamesE",             "FName::GNames mangled"        },
};

constexpr const char* kGNamesSubstrings[] = {
    "NamePool",
    "GNameBlocks",
    "GNames",
    "FNameTable",        // GFNameTableForDebuggerVisualizers_MT and friends
    // intentionally NOT "GName" alone — too noisy, matches FName, FGameName, etc.
};

// =============================================================================
// String literals that NamePoolData / Blocks tend to be referenced near.
// Mirror of kCandidateLiterals for GObjects:
//   * Direct global / type names — sometimes survive stripping as debug
//     residue.
//   * Function-context anchors — strings inside functions that load
//     NamePoolData (FName::FName ctor, FName::ToString, FName::AppendString,
//     FName::GetPlainNameString, etc.). When the type-name literal is dead,
//     these tend to still be referenced.
// =============================================================================
constexpr const char* kCandidateLiteralsGNames[] = {
    // Direct type/global names — usually only survive as .dynstr (debug
    // symbol names), but try anyway. Cheap.
    "NamePoolData",
    "FNamePool",
    "FNameEntry",
    "GNameBlocksDebug",
    "GFNameTableForDebuggerVisualizers_MT",
    "FName::FName",
    "FName::ToString",
    "FName::AppendString",
    "GetPlainNameString",

    // Log / printf format strings inside FName / FNamePool code — these are
    // genuinely embedded in .rodata and survive stripping (the format-string
    // arg has to be passed by-pointer at runtime). Each one corresponds to a
    // log line in UE's FName.cpp / NameTypes.cpp / NamePool.cpp.
    "Failed to find name '%s'",
    "FName entries:",
    "Hash:",
    "MallocBinned",
    "Reserve %d entries",
    "DumpNames",
    "name pool",
    "NamePool",
    "Name table corrupted",

    // Names directly from FNamePool initialization — the canonical "None"
    // entry is followed by "ByteProperty" / "IntProperty" / etc., and
    // "/Script/CoreUObject" is the package the bootstrap classes register
    // to. These literals sometimes survive when the type-name strings don't.
    "/Script/CoreUObject",
    "ByteProperty",
    "IntProperty",
};

// Walk the in-memory .dynsym of libUE4 and call `visit(name, st_value)` for
// every defined symbol. Mirrors FindSymbolDynamic's traversal so we don't
// duplicate the gnu-hash bucket logic — but yields all symbols rather than
// stopping at one match.
template <class Fn>
INTERNAL void IterateDynsym(uintptr_t base, Fn&& visit) noexcept {
    if (base == 0) return;
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(base);
    auto* phdr = reinterpret_cast<Elf64_Phdr*>(base + ehdr->e_phoff);

    Elf64_Dyn*  dyn       = nullptr;
    Elf64_Sym*  dynsym    = nullptr;
    const char* dynstr    = nullptr;
    uint32_t*   gnu_hash  = nullptr;
    size_t      syment    = sizeof(Elf64_Sym);

    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<Elf64_Dyn*>(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return;

    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB:   dynsym   = reinterpret_cast<Elf64_Sym*>  (base + d->d_un.d_ptr); break;
            case DT_STRTAB:   dynstr   = reinterpret_cast<const char*> (base + d->d_un.d_ptr); break;
            case DT_SYMENT:   syment   = d->d_un.d_val;                                       break;
            case DT_GNU_HASH: gnu_hash = reinterpret_cast<uint32_t*>   (base + d->d_un.d_ptr); break;
        }
    }
    if (!dynsym || !dynstr || !gnu_hash) return;

    const uint32_t  nbuckets   = gnu_hash[0];
    const uint32_t  symoffset  = gnu_hash[1];
    const uint32_t  bloom_sz   = gnu_hash[2];
    uint32_t* const buckets    = gnu_hash + 4 + bloom_sz * 2;
    uint32_t* const chains     = buckets + nbuckets;

    uint32_t maxSym = symoffset;
    for (uint32_t b = 0; b < nbuckets; ++b) {
        uint32_t idx = buckets[b];
        if (!idx) continue;
        while (!(chains[idx - symoffset] & 1)) ++idx;
        if (idx > maxSym) maxSym = idx;
    }
    const size_t symcnt = maxSym + 1;

    for (size_t i = 0; i < symcnt; ++i) {
        auto* sym = reinterpret_cast<Elf64_Sym*>(
                reinterpret_cast<uint8_t*>(dynsym) + i * syment);
        if (sym->st_value == 0) continue;

        // Skip functions — code can't be FNamePool data, and the substring
        // matcher otherwise pulls in dozens of FNamePool::Method() symbols.
        const unsigned char type = ELF64_ST_TYPE(sym->st_info);
        if (type == STT_FUNC) continue;

        const char* name = dynstr + sym->st_name;
        visit(name, base + sym->st_value);
    }
}

INTERNAL bool TryNameAddress(const UE4Info& info, uintptr_t addr,
                             const char* via, Result& out) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    bool valid = false;
    guard.Try([&] { valid = UE::NameArray::ProbeNoGuard(addr); });

    char head[200];
    if (valid) {
        out.ok      = true;
        out.method  = Method::Symbol;
        out.address = addr;
        out.offset  = addr - info.base;
        out.detail  = std::string("name-symbol: ") + via;
        std::snprintf(head, sizeof(head),
                      "[name:%s @ 0x%lx] VALIDATED -> offset 0x%lx",
                      via, (unsigned long)addr, (unsigned long)out.offset);
        out.traceLines.emplace_back(head);
        DLOGI("FindGNames_Symbol: %s", head);
        return true;
    }

    std::snprintf(head, sizeof(head),
                  "[name:%s @ 0x%lx] resolved but Probe rejected",
                  via, (unsigned long)addr);
    out.traceLines.emplace_back(head);
    DLOGW("FindGNames_Symbol: %s", head);

    std::vector<std::string> probeReport;
    SafeMemory::ScopedSigSegvGuard reportGuard;
    reportGuard.Try([&] { UE::NameArray::ProbeReport(addr, probeReport); });
    for (const auto& line : probeReport) {
        out.traceLines.emplace_back(std::string("    ") + line);
    }
    return false;
}

INTERNAL bool TryNameSymbol(const UE4Info& info, const char* name,
                            Result& out) noexcept {
    void* sym = FindSymbolDynamic(info.base, name);
    bool fromDisk = false;
    if (sym == nullptr && info.apkPath[0] != 0) {
        sym = FindSymbolDisk(info.apkPath, info.base, name);
        if (sym != nullptr) fromDisk = true;
    }
    if (sym == nullptr) {
        char b[120];
        std::snprintf(b, sizeof(b), "[name:%s] not in symtab", name);
        out.traceLines.emplace_back(b);
        DLOGI("FindGNames_Symbol: %s", b);
        return false;
    }

    char tag[160];
    std::snprintf(tag, sizeof(tag), "%s %s",
                  name, fromDisk ? "(disk)" : "(dynsym)");
    return TryNameAddress(info, reinterpret_cast<uintptr_t>(sym), tag, out);
}

}  // namespace

Result FindGNames_Symbol(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);

        const size_t nExact = sizeof(kGNamesSymbols) / sizeof(kGNamesSymbols[0]);
        DLOGI("FindGNames_Symbol: phase 1 — trying %zu exact names", nExact);
        r.traceLines.emplace_back("phase 1: exact names");
        for (const auto& sn : kGNamesSymbols) {
            if (TryNameSymbol(info, sn.name, r)) return;
        }

        // Phase 2 — fuzzy substring scan over .dynsym so we catch any
        // mangled variant whose length-prefix digits we couldn't predict.
        const size_t nSub = sizeof(kGNamesSubstrings) / sizeof(kGNamesSubstrings[0]);
        DLOGI("FindGNames_Symbol: phase 2 — scanning .dynsym for %zu substrings", nSub);
        r.traceLines.emplace_back("phase 2: substring scan over .dynsym");

        // Collect dynsym symbols that match any keyword. Capped to keep the
        // candidate set bounded on libraries with very large symbol tables.
        constexpr size_t kMaxSubstringHits = 64;
        struct Hit { std::string name; uintptr_t addr; };
        std::vector<Hit> hits;

        IterateDynsym(info.base, [&](const char* name, uintptr_t addr) {
            if (hits.size() >= kMaxSubstringHits) return;
            for (const char* needle : kGNamesSubstrings) {
                if (std::strstr(name, needle) != nullptr) {
                    hits.push_back({ std::string(name), addr });
                    break;
                }
            }
        });

        char b[200];
        std::snprintf(b, sizeof(b), "[.dynsym] %zu symbols matched substrings", hits.size());
        r.traceLines.emplace_back(b);
        DLOGI("FindGNames_Symbol: %s", b);

        for (const auto& h : hits) {
            if (TryNameAddress(info, h.addr, h.name.c_str(), r)) return;
        }

        r.detail = "all GNames symbol/substring attempts exhausted (see trace)";
    });
}

namespace {

// =============================================================================
// FName function-walk strategy
// -----------------------------------------------------------------------------
// When NamePoolData isn't a discoverable symbol on its own, the *functions*
// that operate on FName almost always are — `FName::FName(wide,EFindName)`,
// `FName::AppendString`, `FName::ToString`, `FName::GetPlainNameString`. Each
// of those functions loads the global FNamePool pointer in its prologue via
// an ADRP+ADD pair (sometimes ADRP+LDR for indirect refs). For ctors that
// have to take a slow path through `FNamePool::Find()`, the ADRP may land
// after a BL into a helper — so we also follow B/BL once and scan the
// callee's first ~64 instructions.
//
// Every candidate target is run through ValidateGNamesCandidate (probe
// "None" at Blocks[0]), so if a function uses ADRP+ADD for an unrelated
// global (a vtable, a TLS slot, a string) we skip past it.
// =============================================================================

constexpr SymbolName kFNameFunctionSymbols[] = {
    // FName ctors (wide char16_t — `Ds` mangling):
    //   C1 = complete, C2 = base — both emitted as separate symbols by clang.
    //   The `9EFindName` / `14EFindName` length-prefix differs across UE
    //   versions because the typedef name (e.g. `EFindName` -> `EFindName`)
    //   gets mangled by-length: 9 = "EFindName" alone, 14 = with namespace.
    { "_ZN5FNameC1EPKDs14EFindName",          "FName(wide,EFindName) C1 v14" },
    { "_ZN5FNameC2EPKDs14EFindName",          "FName(wide,EFindName) C2 v14" },
    { "_ZN5FNameC1EPKDs9EFindName",           "FName(wide,EFindName) C1 v9"  },
    { "_ZN5FNameC2EPKDs9EFindName",           "FName(wide,EFindName) C2 v9"  },
    // char ctors
    { "_ZN5FNameC1EPKc9EFindName",            "FName(char,EFindName) C1 v9"  },
    { "_ZN5FNameC2EPKc9EFindName",            "FName(char,EFindName) C2 v9"  },
    { "_ZN5FNameC1EPKc14EFindName",           "FName(char,EFindName) C1 v14" },
    { "_ZN5FNameC2EPKc14EFindName",           "FName(char,EFindName) C2 v14" },
    // Const member fns — preferred anchors: small bodies, ADRP+ADD usually
    // in the first 4-8 instructions. Less branch noise than ctors.
    { "_ZNK5FName12AppendStringER7FString",   "FName::AppendString"          },
    { "_ZNK5FName8ToStringEv",                "FName::ToString"              },
    { "_ZNK5FName18GetPlainNameStringEv",     "FName::GetPlainNameString"    },
};

constexpr size_t kFuncWalkBytes      = 0x200;       // window inside fn
constexpr size_t kFuncBranchScanBytes = 0x100;      // window inside callee

// Walk `bytes` of code at `start` looking for ADRP+ADD/LDR pairs whose
// resolved address lands in writable AND validates. If nothing direct,
// follow each B/BL within the window into the callee's first
// kFuncBranchScanBytes and try again. Returns the validated address or 0.
INTERNAL uintptr_t WalkFunctionForCandidate(uintptr_t start, size_t bytes,
                                            const std::vector<Segment>& wSegs,
                                            const std::vector<Segment>& xSegs,
                                            Validator validate) noexcept {
    // depth=1 is enough here — Func-walk starts AT an FName function, so the
    // ADRP NamePoolData is at most one BL away (FNamePool::Find called from
    // FName::FName / AppendString / ToString). String-ref needs depth=2
    // because it starts at a literal-ref site one extra hop further out.
    return ScanForRefWithBranchFollow(
            start, static_cast<int>(bytes / 4),
            static_cast<int>(kFuncBranchScanBytes / 4),
            /*depth*/ 1, wSegs, xSegs, validate);
}

INTERNAL bool FuncWalkScan(const UE4Info& info, Result& out) noexcept {
    auto trace = [&](const char* fmt, ...) {
        char b[224];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        out.traceLines.emplace_back(b);
        DLOGI("[funcwalk] %s", b);
    };

    auto wSegs = CollectSegments(info.base, /*req*/ PF_W,        /*forbid*/ 0);
    auto xSegs = CollectSegments(info.base, /*req*/ PF_R | PF_X, /*forbid*/ PF_W);
    if (wSegs.empty() || xSegs.empty()) {
        out.detail = "func-walk: missing writable / executable segments";
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;
    bool anyResolved = false;

    for (const auto& sn : kFNameFunctionSymbols) {
        void* fn       = FindSymbolDynamic(info.base, sn.name);
        bool  fromDisk = false;
        if (fn == nullptr && info.apkPath[0] != 0) {
            fn = FindSymbolDisk(info.apkPath, info.base, sn.name);
            if (fn != nullptr) fromDisk = true;
        }
        if (fn == nullptr) {
            trace("[%s] not in symtab (%s)", sn.name, sn.note);
            continue;
        }
        anyResolved = true;
        trace("[%s @ 0x%lx %s] resolved (%s)",
              sn.name, (unsigned long)fn,
              fromDisk ? "(disk)" : "(dynsym)", sn.note);

        uintptr_t hit = 0;
        guard.Try([&] {
            hit = WalkFunctionForCandidate(reinterpret_cast<uintptr_t>(fn),
                                           kFuncWalkBytes, wSegs, xSegs,
                                           &ValidateGNamesCandidate);
        });
        if (!hit) {
            trace("    no validating ADRP+ADD/LDR within 0x%zx bytes (incl. one BL hop)",
                  kFuncWalkBytes);
            continue;
        }

        out.ok      = true;
        out.method  = Method::CodeRef;
        out.address = hit;
        out.offset  = hit - info.base;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "func-walk via %s @ 0x%lx -> NamePoolData @ 0x%lx (offset 0x%lx)",
                      sn.name, (unsigned long)fn,
                      (unsigned long)hit, (unsigned long)out.offset);
        out.detail = buf;
        trace("    VALIDATED: %s", buf);
        return true;
    }

    out.detail = anyResolved
            ? "func-walk: FName function symbols present, none yielded a valid NamePoolData"
            : "func-walk: no FName function symbols in dynsym";
    return false;
}

}  // namespace

Result FindGNames_FuncWalk(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        FuncWalkScan(info, r);
    });
}

Result FindGNames_StringRef(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        StringRefScan(info, kCandidateLiteralsGNames,
                      sizeof(kCandidateLiteralsGNames) / sizeof(kCandidateLiteralsGNames[0]),
                      &ValidateGNamesCandidate, "NamePoolData", r);
    });
}

Result FindGNames_CodeRef(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        CodeRefScan(info, &ValidateGNamesCandidate, "NamePoolData", r);
    });
}

Result FindGNames_CodeRefQuick(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        CodeRefQuickScan(info, &ValidateGNamesCandidate, "NamePoolData", r);
    });
}

Result FindGNames_Pattern(const UE4Info& info, const char* patternSig) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        PatternScan(info, patternSig, &ValidateGNamesCandidate, "NamePoolData", r);
    });
}

Result FindGNames_BssScan(const UE4Info& info) noexcept {
    return TimedRun([&](Result& r) {
        if (info.base == 0) { r.detail = "libUE4 not located"; return; }
        EnsureInitialized(info);
        BssScan(info, &ValidateGNamesCandidate, "NamePoolData", r);
    });
}

Result FindGNames(const UE4Info& info) noexcept {
    Result r = FindGNames_Symbol(info);
    if (r.ok) return r;
    r = FindGNames_FuncWalk(info);
    if (r.ok) return r;
    r = FindGNames_StringRef(info);
    if (r.ok) return r;
    r = FindGNames_CodeRefQuick(info);
    if (r.ok) return r;
    r = FindGNames_CodeRef(info);
    if (r.ok) return r;
    return FindGNames_BssScan(info);
}

CodeRefProgress CodeRefSnapshot() noexcept {
    std::lock_guard<std::mutex> lk(g_codeRefProgressMu);
    return g_codeRefProgress;
}

const char* MethodName(Method m) noexcept {
    switch (m) {
        case Method::None:      return "none";
        case Method::Symbol:    return "symbol";
        case Method::StringRef: return "string-ref";
        case Method::CodeRefQuick: return "code-ref-quick";
        case Method::CodeRef:   return "code-ref";
        case Method::Pattern:   return "pattern";
        case Method::BssScan:   return "bss-scan";
        case Method::DeepScan:  return "deep-scan";
        case Method::Manual:    return "manual";
    }
    return "?";
}

}  // namespace OffsetFinder
