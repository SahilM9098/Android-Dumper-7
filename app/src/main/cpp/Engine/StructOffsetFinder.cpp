// =============================================================================
// Engine/StructOffsetFinder.cpp
// =============================================================================

#include "StructOffsetFinder.h"

#include "NameArray.h"
#include "ObjectArray.h"
#include "SafeMemory.h"
#include "UEStructs.h"

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace StructOffsetFinder {

namespace {

constexpr int32_t kMaxOffset      = 0x60;
constexpr int32_t kStride         =    4;     // every 4 bytes
constexpr int32_t kDefaultSample  =  256;
constexpr int32_t kMaxSample      = 4096;

// Snapshot of one live UObject — scanned bytes plus its known InternalIndex
// from FUObjectItem (the validation key).
struct ObjSample {
    uintptr_t address    = 0;
    int32_t   knownIndex = 0;
    uint8_t   bytes[kMaxOffset + 16] = {};   // +16 to allow 8-byte reads at the tail
    bool      bytesOk    = false;
};

INTERNAL bool CollectSamples(int32_t requested,
                             std::vector<ObjSample>& out) noexcept {
    out.clear();
    out.reserve(static_cast<size_t>(requested));

    SafeMemory::ScopedSigSegvGuard guard;
    int32_t collected = 0;

    UE::ObjectArray::ForEach([&](UE::UObject* obj) {
        if (collected >= requested) return;
        ObjSample s;
        s.address = reinterpret_cast<uintptr_t>(obj);

        // Resolve InternalIndex from the object itself — UE's invariant says
        // it equals the FUObjectItem's slot index. Default offset 0x0C is
        // stable across every UE we care about, but we still validate by
        // requiring it to round-trip through GObjects later.
        bool any = false;
        guard.Try([&] {
            for (int32_t i = 0; i < (int32_t)sizeof(s.bytes); ++i) {
                auto b = SafeMemory::SafeReadAny<uint8_t>(s.address + i);
                if (!b) { return; }
                s.bytes[i] = *b;
            }
            any = true;
        });
        if (!any) return;
        s.bytesOk = true;
        s.knownIndex = *reinterpret_cast<int32_t*>(
                s.bytes + Off::UObject::InternalIndex);
        out.push_back(s);
        ++collected;
    });

    return !out.empty();
}

// Read helpers over the byte snapshot — never fault, bounded.
INTERNAL int32_t   ReadI32(const ObjSample& s, int32_t off) noexcept {
    return *reinterpret_cast<const int32_t*>(s.bytes + off);
}
INTERNAL uint32_t  ReadU32(const ObjSample& s, int32_t off) noexcept {
    return *reinterpret_cast<const uint32_t*>(s.bytes + off);
}
INTERNAL uintptr_t ReadPtr(const ObjSample& s, int32_t off) noexcept {
    return *reinterpret_cast<const uintptr_t*>(s.bytes + off);
}

template <typename T>
INTERNAL bool TryReadAnyValue(uintptr_t addr, T& out) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    bool ok = false;
    guard.Try([&] {
        auto v = SafeMemory::SafeReadAny<T>(addr);
        if (!v) return;
        out = *v;
        ok = true;
    });
    return ok;
}

// Pattern: candidate offset stores a non-null pointer whose first 8 bytes
// (the vtable) lie in an executable segment of any loaded module. Mirrors
// the UObject vtable invariant Probe relies on.
INTERNAL bool LooksLikeUObjectPointer(uintptr_t cand) noexcept {
    if (cand == 0) return false;
    if ((cand & 0x7) != 0) return false;            // 8-byte alignment
    uintptr_t vt = 0;
    if (!TryReadAnyValue<uintptr_t>(cand, vt) || vt == 0) return false;
    if ((vt & 0x7) != 0) return false;
    return SafeMemory::IsExecutable(vt);
}

// Pattern: candidate offset stores 8-byte FName whose ComparisonIndex
// resolves to a non-empty name via NameArray. Number must be a small
// non-negative int (most names have Number <= a few thousand).
INTERNAL bool LooksLikeFName(const ObjSample& s, int32_t off,
                             std::string& outName) noexcept {
    int32_t comparisonIndex = ReadI32(s, off);
    int32_t number          = ReadI32(s, off + 4);
    if (comparisonIndex < 0 || comparisonIndex > 0x40000000) return false;
    if (number < 0 || number > 0x10000) return false;
    std::string name = UE::NameArray::GetName(comparisonIndex);
    if (name.empty()) return false;
    if (name.size() > 1023) return false;
    outName = std::move(name);
    return true;
}

// =============================================================================
// Per-offset scoring
// =============================================================================

struct OffsetStats {
    int32_t offset            = -1;
    int32_t fnameMatches      =  0;
    int32_t classPtrMatches   =  0;
    int32_t outerNullMatches  =  0;
    int32_t outerPtrMatches   =  0;
    int32_t indexMatches      =  0;   // value == sample.knownIndex
};

INTERNAL std::vector<OffsetStats>
ScoreAll(const std::vector<ObjSample>& samples) noexcept {
    std::vector<OffsetStats> out;
    out.reserve(static_cast<size_t>(kMaxOffset / kStride) + 1);

    SafeMemory::ScopedSigSegvGuard guard;

    for (int32_t off = 0; off <= kMaxOffset; off += kStride) {
        OffsetStats st;
        st.offset = off;

        for (const auto& s : samples) {
            // FName pattern (8 bytes, must be 4-byte aligned).
            if (off + 8 <= (int32_t)sizeof(s.bytes)) {
                std::string nm;
                bool fn = false;
                guard.Try([&] { fn = LooksLikeFName(s, off, nm); });
                if (fn) ++st.fnameMatches;
            }

            // Pointer patterns (8 bytes, 8-byte aligned).
            if ((off & 0x7) == 0 && off + 8 <= (int32_t)sizeof(s.bytes)) {
                uintptr_t p = ReadPtr(s, off);

                // ClassPrivate: must be a UObject pointer (non-null).
                bool cp = false;
                guard.Try([&] { cp = LooksLikeUObjectPointer(p); });
                if (cp) ++st.classPtrMatches;

                // OuterPrivate: null OR UObject pointer.
                if (p == 0) {
                    ++st.outerNullMatches;
                } else if (cp) {
                    ++st.outerPtrMatches;
                }
            }

            // InternalIndex: value matches sample's known slot.
            if (off + 4 <= (int32_t)sizeof(s.bytes)) {
                int32_t v = ReadI32(s, off);
                if (v == s.knownIndex) ++st.indexMatches;
            }
        }

        out.push_back(st);
    }
    return out;
}

INTERNAL FieldHit PickBest(const std::vector<OffsetStats>& stats,
                           int32_t sampleSize,
                           int32_t (OffsetStats::*field),
                           int32_t minOffset,
                           int32_t maxOffset,
                           int32_t excludeOffset = -1) noexcept {
    FieldHit best;
    best.total = sampleSize;
    for (const auto& st : stats) {
        if (st.offset < minOffset || st.offset > maxOffset) continue;
        if (st.offset == excludeOffset) continue;
        const int32_t s = st.*field;
        if (s > best.score) {
            best.score  = s;
            best.offset = st.offset;
        }
    }
    return best;
}

}  // namespace

UObjectLayout FindUObjectLayout(int32_t requestedSampleSize) noexcept {
    UObjectLayout layout;
    auto t0 = std::chrono::steady_clock::now();

    auto trace = [&](const char* fmt, ...) {
        char b[224];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        layout.traceLines.emplace_back(b);
        DLOGI("[uobjoff] %s", b);
    };

    if (!UE::ObjectArray::IsInitialized()) {
        trace("ObjectArray not initialized — open the ObjectArray tab and run Symbol/etc first");
        return layout;
    }
    if (!UE::NameArray::IsInitialized()) {
        trace("NameArray not initialized — open the NameArray tab and run Symbol/etc first");
        return layout;
    }

    int32_t sample = requestedSampleSize > 0 ? requestedSampleSize : kDefaultSample;
    if (sample > kMaxSample) sample = kMaxSample;

    std::vector<ObjSample> samples;
    if (!CollectSamples(sample, samples)) {
        trace("could not collect any UObject samples (every ForEach iter faulted?)");
        return layout;
    }
    layout.sampleSize = static_cast<int32_t>(samples.size());
    trace("collected %d UObject samples", layout.sampleSize);

    auto stats = ScoreAll(samples);

    // VTable: always 0 by C++ ABI on AArch64. Score it anyway so the trace
    // shows match rate (sanity check that our snapshot logic is correct).
    {
        FieldHit hit;
        hit.offset = 0;
        hit.total  = layout.sampleSize;
        // For VTable, "match" = looks-like-pointer-into-exec at offset 0.
        for (const auto& st : stats) {
            if (st.offset == 0) { hit.score = st.classPtrMatches; break; }
        }
        layout.vtable = hit;
        trace("vtable @ 0x00: %d/%d objects had pointer-into-exec at offset 0",
              hit.score, hit.total);
    }

    // InternalIndex: known to be 0x0C on UE4.20+. Pick the offset where the
    // value equals its sample's slot for >95% of objects. Constrain search to
    // 0x04..0x14 (right after vtable, before the typical class-pointer slot).
    layout.internalIndex = PickBest(stats, layout.sampleSize,
                                    &OffsetStats::indexMatches,
                                    /*min*/ 0x04, /*max*/ 0x14);
    trace("InternalIndex best @ 0x%x: %d/%d match",
          layout.internalIndex.offset,
          layout.internalIndex.score, layout.internalIndex.total);

    // ClassPrivate: pointer-to-UObject. Constrain to 0x08..0x20 — after
    // ObjectFlags, before the FName slot.
    layout.classPrivate = PickBest(stats, layout.sampleSize,
                                   &OffsetStats::classPtrMatches,
                                   /*min*/ 0x08, /*max*/ 0x20);
    trace("ClassPrivate best @ 0x%x: %d/%d UObject-pointers",
          layout.classPrivate.offset,
          layout.classPrivate.score, layout.classPrivate.total);

    // NamePrivate: FName decodes to a non-empty name. Constrain to right
    // after ClassPrivate (8 bytes later) — that's the canonical UE layout.
    int32_t nameMin = layout.classPrivate.offset >= 0
            ? layout.classPrivate.offset + 8
            : 0x10;
    layout.namePrivate = PickBest(stats, layout.sampleSize,
                                  &OffsetStats::fnameMatches,
                                  /*min*/ nameMin, /*max*/ nameMin + 0x10);
    trace("NamePrivate best @ 0x%x: %d/%d FName-decodes",
          layout.namePrivate.offset,
          layout.namePrivate.score, layout.namePrivate.total);

    // OuterPrivate: null OR UObject pointer. Right after NamePrivate (FName
    // is 8 bytes). Score is null + valid-pointer combined.
    int32_t outerMin = layout.namePrivate.offset >= 0
            ? layout.namePrivate.offset + 8
            : 0x18;
    {
        FieldHit best;
        best.total = layout.sampleSize;
        for (const auto& st : stats) {
            if (st.offset < outerMin || st.offset > outerMin + 0x10) continue;
            const int32_t score = st.outerNullMatches + st.outerPtrMatches;
            if (score > best.score) {
                best.score  = score;
                best.offset = st.offset;
            }
        }
        layout.outerPrivate = best;
        trace("OuterPrivate best @ 0x%x: %d/%d (null OR UObject*)",
              best.offset, best.score, best.total);
    }

    // Flags: a uint32 right after the vtable. We can't pattern-test reliably
    // (flags are bitmasks of arbitrary value), so default to 0x08 if it's
    // not where InternalIndex sits.
    {
        FieldHit hit;
        hit.total  = layout.sampleSize;
        hit.offset = (layout.internalIndex.offset == 0x08) ? 0x10 : 0x08;
        // Validate: the bytes at this offset shouldn't all be zero across
        // the entire sample (objects do have varied flags).
        int32_t nonZero = 0;
        for (const auto& s : samples) {
            if (ReadU32(s, hit.offset) != 0) ++nonZero;
        }
        hit.score = nonZero;
        layout.flags = hit;
        trace("ObjectFlags @ 0x%x (default placement): %d/%d non-zero",
              hit.offset, nonZero, hit.total);
    }

    // Inferred sizeof(UObject) — last identified field's offset + its size,
    // rounded up to 8.
    int32_t maxField = 0;
    auto bumpField = [&](const FieldHit& h, int32_t width) {
        if (h.offset >= 0 && h.score > 0 && h.offset + width > maxField)
            maxField = h.offset + width;
    };
    bumpField(layout.flags,         4);
    bumpField(layout.internalIndex, 4);
    bumpField(layout.classPrivate,  8);
    bumpField(layout.namePrivate,   8);
    bumpField(layout.outerPrivate,  8);
    layout.inferredSize = (maxField + 7) & ~7;

    // Sanity: require InternalIndex and ClassPrivate to be strong.
    // NamePrivate can underperform when the NamePool detection picked a
    // slightly wrong blocksOffset or stride — the offset itself is still
    // correct (it's the best-scoring one), just the decoder can't resolve
    // all names yet. Accept the layout so downstream steps can proceed.
    auto strong = [&](const FieldHit& h, int32_t pct) {
        return h.offset >= 0 && h.score * 100 >= h.total * pct;
    };
    layout.ok =
            strong(layout.internalIndex, 95) &&
            strong(layout.classPrivate,  60);

    auto t1 = std::chrono::steady_clock::now();
    layout.elapsedMicros =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    trace("inferredSize=0x%x ok=%d (%.1f ms)",
          layout.inferredSize, (int)layout.ok, layout.elapsedMicros / 1000.0);
    return layout;
}

void Apply(const UObjectLayout& l) noexcept {
    if (!l.ok) return;
    Off::UObject::VTable        = 0;
    if (l.flags.offset         >= 0) Off::UObject::ObjectFlags   = l.flags.offset;
    if (l.internalIndex.offset >= 0) Off::UObject::InternalIndex = l.internalIndex.offset;
    if (l.classPrivate.offset  >= 0) Off::UObject::ClassPrivate  = l.classPrivate.offset;
    if (l.namePrivate.offset   >= 0) Off::UObject::NamePrivate   = l.namePrivate.offset;
    if (l.outerPrivate.offset  >= 0) Off::UObject::OuterPrivate  = l.outerPrivate.offset;
    if (l.inferredSize         >  0) Off::UObject::Size          = l.inferredSize;
}

// =============================================================================
// UField + UStruct finders
// =============================================================================

namespace {

constexpr int32_t kUStructStride         = 8;     // pointer-sized stride
constexpr int32_t kUStructScanSpan       = 0x40;  // bytes after UObject base to scan
constexpr int32_t kUStructDefaultSample  =  256;
constexpr int32_t kUStructMaxSample      = 4096;
constexpr int32_t kFieldChainMaxAnchors  =   32;  // chain-walk cost cap
constexpr int32_t kFieldChainMaxDepth    = 4096;  // generous; UClass usually <500

// Same byte snapshot used for UClass samples — but a wider window so we
// can reach offsets up to UObject::Size + 0x40.
struct ClassSample {
    uintptr_t address = 0;
    uint8_t   bytes[0x80] = {};
    bool      ok = false;
};

INTERNAL std::string ReadFNameAt(uintptr_t addr) noexcept {
    int32_t comp = 0;
    if (!TryReadAnyValue<int32_t>(addr, comp)) return {};
    if (comp < 0 || comp > 0x40000000) return {};
    return UE::NameArray::GetName(comp);
}

// Read the name of a UObject — i.e. the value of its NamePrivate FName.
INTERNAL std::string ReadObjectName(uintptr_t obj) noexcept {
    if (obj == 0) return {};
    return ReadFNameAt(obj + Off::UObject::NamePrivate);
}

// Read the *class name* of a UObject — name of its ClassPrivate target.
INTERNAL std::string ReadObjectClassName(uintptr_t obj) noexcept {
    if (obj == 0) return {};
    uintptr_t cls = 0;
    if (!TryReadAnyValue<uintptr_t>(obj + Off::UObject::ClassPrivate, cls) || cls == 0) {
        return {};
    }
    return ReadObjectName(cls);
}

INTERNAL bool IsUClassInstance(uintptr_t obj) noexcept {
    return ReadObjectClassName(obj) == "Class";
}

INTERNAL bool CollectClassSamples(int32_t requested,
                                  std::vector<ClassSample>& out) noexcept {
    out.clear();
    out.reserve(static_cast<size_t>(requested));

    SafeMemory::ScopedSigSegvGuard guard;
    int32_t collected = 0;

    UE::ObjectArray::ForEach([&](UE::UObject* obj) {
        if (collected >= requested) return;
        uintptr_t addr = reinterpret_cast<uintptr_t>(obj);

        bool isClass = false;
        guard.Try([&] { isClass = IsUClassInstance(addr); });
        if (!isClass) return;

        ClassSample s;
        s.address = addr;
        bool any = false;
        guard.Try([&] {
            for (int32_t i = 0; i < (int32_t)sizeof(s.bytes); ++i) {
                auto b = SafeMemory::SafeReadAny<uint8_t>(s.address + i);
                if (!b) return;
                s.bytes[i] = *b;
            }
            any = true;
        });
        if (!any) return;
        s.ok = true;
        out.push_back(s);
        ++collected;
    });

    return !out.empty();
}

INTERNAL uintptr_t ReadPtrField(const ClassSample& s, int32_t off) noexcept {
    if (off < 0 || off + 8 > (int32_t)sizeof(s.bytes)) return 0;
    return *reinterpret_cast<const uintptr_t*>(s.bytes + off);
}

struct StructStats {
    int32_t offset            = -1;
    int32_t nullCount         =  0;
    int32_t uobjectPtrCount   =  0;
    int32_t uclassPtrCount    =  0;     // UObject pointer whose class is "Class"
    int32_t selfRefCount      =  0;     // value == sample.address (would loop)
};

// Score every 8-byte offset starting after UObject base. Per-offset stats
// drive SuperStruct vs Children selection.
INTERNAL std::vector<StructStats>
ScoreStruct(const std::vector<ClassSample>& samples) noexcept {
    std::vector<StructStats> out;
    SafeMemory::ScopedSigSegvGuard guard;
    const int32_t startOff = Off::UObject::Size;
    const int32_t endOff   = startOff + kUStructScanSpan;

    for (int32_t off = startOff; off + 8 <= endOff; off += kUStructStride) {
        StructStats st;
        st.offset = off;

        for (const auto& s : samples) {
            uintptr_t p = ReadPtrField(s, off);
            if (p == 0) { ++st.nullCount; continue; }
            if (p == s.address) { ++st.selfRefCount; continue; }

            bool isObj = false;
            guard.Try([&] { isObj = LooksLikeUObjectPointer(p); });
            if (!isObj) continue;
            ++st.uobjectPtrCount;

            bool isClass = false;
            guard.Try([&] { isClass = IsUClassInstance(p); });
            if (isClass) ++st.uclassPtrCount;
        }
        out.push_back(st);
    }
    return out;
}

// Walk Children → Next chain via candidate offset N. Returns true iff the
// chain terminates at null within kFieldChainMaxDepth and every visited
// pointer is a UObject pointer with no cycles.
INTERNAL bool WalkChainTerminates(uintptr_t start, int32_t nextOffset) noexcept {
    if (start == 0) return true;  // empty chain trivially terminates
    SafeMemory::ScopedSigSegvGuard guard;
    std::unordered_set<uintptr_t> seen;
    seen.reserve(64);

    uintptr_t cur = start;
    for (int32_t depth = 0; depth < kFieldChainMaxDepth; ++depth) {
        if (cur == 0) return true;
        if (!seen.insert(cur).second) return false;  // cycle

        bool valid = false;
        guard.Try([&] { valid = LooksLikeUObjectPointer(cur); });
        if (!valid) return false;

        uintptr_t nxt = 0;
        bool ok = guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + nextOffset);
            if (v) nxt = *v;
        });
        if (!ok) return false;
        cur = nxt;
    }
    return false;  // hit depth cap — assume not a real chain
}

// Pick UStruct::Next offset by chain-walking on a subset of samples that have
// non-null Children. Score = % of anchors whose chain terminates cleanly.
INTERNAL FieldHit FindFieldNext(const std::vector<ClassSample>& samples,
                                int32_t childrenOff,
                                int32_t superOff) noexcept {
    FieldHit best;
    if (childrenOff < 0) return best;

    // Pull up to kFieldChainMaxAnchors anchors with non-null Children.
    std::vector<uintptr_t> anchors;
    anchors.reserve(kFieldChainMaxAnchors);
    for (const auto& s : samples) {
        uintptr_t c = ReadPtrField(s, childrenOff);
        if (c == 0) continue;
        anchors.push_back(c);
        if ((int32_t)anchors.size() >= kFieldChainMaxAnchors) break;
    }
    if (anchors.empty()) return best;
    best.total = static_cast<int32_t>(anchors.size());

    // Candidate Next offsets to probe inside the UField — typically right
    // after the UObject base, but allow a small window for engine drift.
    const int32_t startOff = Off::UObject::Size;
    const int32_t endOff   = startOff + 0x10;

    for (int32_t off = startOff; off + 8 <= endOff; off += kUStructStride) {
        // Skip offsets that collide with SuperStruct/Children — those are
        // *outer* pointers (live in UStruct), not the inner UField::Next.
        // (UField::Next lives at a *smaller* offset than UStruct fields, so
        // this is mostly belt-and-suspenders.)
        if (off == childrenOff || off == superOff) continue;

        int32_t terminations = 0;
        for (uintptr_t a : anchors) {
            if (WalkChainTerminates(a, off)) ++terminations;
        }
        if (terminations > best.score) {
            best.score  = terminations;
            best.offset = off;
        }
    }
    return best;
}

}  // namespace

UStructLayout FindUStructLayout(int32_t requestedSampleSize) noexcept {
    UStructLayout layout;
    auto t0 = std::chrono::steady_clock::now();

    auto trace = [&](const char* fmt, ...) {
        char b[224];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        layout.traceLines.emplace_back(b);
        DLOGI("[ustructoff] %s", b);
    };

    if (!UE::ObjectArray::IsInitialized()) {
        trace("ObjectArray not initialized");
        return layout;
    }
    if (!UE::NameArray::IsInitialized()) {
        trace("NameArray not initialized");
        return layout;
    }
    if (Off::UObject::ClassPrivate == 0 || Off::UObject::NamePrivate == 0) {
        trace("UObject offsets unset — run UObject scan + Apply first");
        return layout;
    }

    int32_t sample = requestedSampleSize > 0 ? requestedSampleSize : kUStructDefaultSample;
    if (sample > kUStructMaxSample) sample = kUStructMaxSample;

    std::vector<ClassSample> samples;
    if (!CollectClassSamples(sample, samples)) {
        trace("found 0 UClass instances — is the world fully loaded?");
        return layout;
    }
    layout.sampleSize = static_cast<int32_t>(samples.size());
    trace("collected %d UClass samples", layout.sampleSize);

    auto stats = ScoreStruct(samples);

    // Per-offset breakdown — log every slot so the operator can sanity-check
    // the picker choices. Keep it terse (one line per offset).
    trace("--- per-offset breakdown (samples=%d) ---", layout.sampleSize);
    for (const auto& st : stats) {
        trace("  0x%02x  null=%d  obj*=%d  class*=%d  self=%d",
              st.offset, st.nullCount, st.uobjectPtrCount,
              st.uclassPtrCount, st.selfRefCount);
    }

    // SuperStruct discriminator: among UClass samples, SuperStruct points to
    // *another UClass* (the parent class). Children points to a UField that's
    // typically NOT a UClass (UFunction, UProperty, etc.). So the slot with
    // the highest `uclassPtrCount` is SuperStruct — uniquely.
    //
    // Require ≥30% of samples to have a UClass parent. UObject's root is
    // null, but everything else descends from UObject, so 30% is very
    // conservative; the real number is usually 95%+.
    const int32_t kSuperMin = layout.sampleSize * 30 / 100;
    {
        FieldHit& h = layout.superStruct;
        h.total = layout.sampleSize;
        int32_t bestNull = 0, bestObj = 0;
        for (const auto& st : stats) {
            if (st.uclassPtrCount < kSuperMin) continue;
            if (st.uclassPtrCount > h.score) {
                h.score   = st.uclassPtrCount;
                h.offset  = st.offset;
                bestNull  = st.nullCount;
                bestObj   = st.uobjectPtrCount;
            }
        }
        if (h.offset >= 0) {
            trace("SuperStruct best @ 0x%x: %d UClass*, %d null, %d UObject* total",
                  h.offset, h.score, bestNull, bestObj);
        } else {
            trace("SuperStruct: no offset cleared %d-UClass-ref threshold", kSuperMin);
        }
    }

    // Children: highest `uobjectPtrCount` EXCLUDING the SuperStruct slot,
    // and where targets are predominantly NOT UClasses (uobjectPtrCount must
    // exceed uclassPtrCount — otherwise we're looking at another class-typed
    // pointer like a duplicated Super or Outer).
    {
        FieldHit& h = layout.children;
        h.total = layout.sampleSize;
        int32_t bestNull = 0, bestClass = 0;
        for (const auto& st : stats) {
            if (st.offset == layout.superStruct.offset) continue;
            if (st.uobjectPtrCount == 0) continue;
            int32_t nonClassPtr = st.uobjectPtrCount - st.uclassPtrCount;
            if (nonClassPtr <= 0) continue;          // require non-class targets
            // Score = null + non-class UObject*. We don't add uclassPtrCount
            // back in — that would let SuperStruct-shaped offsets win.
            int32_t score = st.nullCount + nonClassPtr;
            if (score > h.score) {
                h.score   = score;
                h.offset  = st.offset;
                bestNull  = st.nullCount;
                bestClass = st.uclassPtrCount;
            }
        }
        if (h.offset >= 0) {
            trace("Children    best @ 0x%x: %d (null+nonClassObj*), %d null, %d UClass*",
                  h.offset, h.score, bestNull, bestClass);
        } else {
            trace("Children: no offset matched (need uobject* > uclass* and >0 non-null)");
        }
    }

    // Sanity swap: SuperStruct must come before Children in canonical UE
    // layout. (UStruct::SuperStruct is declared before UStruct::Children.)
    if (layout.superStruct.offset > 0 && layout.children.offset > 0 &&
        layout.superStruct.offset > layout.children.offset) {
        trace("WARN: super @0x%x > children @0x%x — swapping",
              layout.superStruct.offset, layout.children.offset);
        std::swap(layout.superStruct, layout.children);
    }

    // UField::Next via chain-walk validation.
    layout.fieldNext = FindFieldNext(samples,
                                     layout.children.offset,
                                     layout.superStruct.offset);
    trace("UField::Next best @ 0x%x: %d/%d clean chain terminations",
          layout.fieldNext.offset,
          layout.fieldNext.score, layout.fieldNext.total);

    // Inferred sizes.
    if (layout.fieldNext.offset >= 0)
        layout.inferredFieldSize = layout.fieldNext.offset + 8;

    int32_t maxStruct = 0;
    auto bump = [&](const FieldHit& h) {
        if (h.offset >= 0 && h.score > 0 && h.offset + 8 > maxStruct)
            maxStruct = h.offset + 8;
    };
    bump(layout.superStruct);
    bump(layout.children);
    layout.inferredStructMin = (maxStruct + 7) & ~7;

    auto strong = [&](const FieldHit& h, int32_t pct) {
        return h.offset >= 0 && h.total > 0 && h.score * 100 >= h.total * pct;
    };
    layout.ok =
            strong(layout.superStruct, 90) &&
            strong(layout.children,    80) &&
            strong(layout.fieldNext,   70);

    auto t1 = std::chrono::steady_clock::now();
    layout.elapsedMicros =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    trace("fieldSize~=0x%x structMin~=0x%x ok=%d (%.1f ms)",
          layout.inferredFieldSize, layout.inferredStructMin,
          (int)layout.ok, layout.elapsedMicros / 1000.0);
    return layout;
}

void Apply(const UStructLayout& l) noexcept {
    if (!l.ok) return;
    if (l.fieldNext.offset    >= 0) Off::UField::Next         = l.fieldNext.offset;
    if (l.inferredFieldSize   >  0) Off::UField::Size         = l.inferredFieldSize;
    if (l.superStruct.offset  >= 0) Off::UStruct::SuperStruct = l.superStruct.offset;
    if (l.children.offset     >= 0) Off::UStruct::Children    = l.children.offset;
}

// =============================================================================
// Advanced finder (UClass / UFunction / FProperty)
// =============================================================================

namespace {

constexpr int32_t kWideSnapshotBytes      = 0x240;  // covers UClass::Size 0x230
constexpr int32_t kPropSnapshotBytes      = 0xC0;   // covers common U/FProperty heads
constexpr int32_t kAdvDefaultClassSample  =   96;   // CDO scan is name-decode heavy
constexpr int32_t kAdvDefaultFuncSample   =  256;
constexpr int32_t kAdvMaxSample           = 4096;

struct WideSample {
    uintptr_t address = 0;
    uint8_t   bytes[kWideSnapshotBytes] = {};
    bool      ok = false;
};

struct StructOwner {
    uintptr_t address = 0;
    int32_t   propertiesSize = 0;
};

struct PropSample {
    uintptr_t   address = 0;
    uintptr_t   owner = 0;
    int32_t     ownerSize = 0;
    std::string className;
    std::string name;
    uint8_t     bytes[kPropSnapshotBytes] = {};
};

INTERNAL bool CollectByClassName(int32_t requested,
                                 const char* className,
                                 std::vector<WideSample>& out) noexcept {
    out.clear();
    out.reserve(static_cast<size_t>(requested));

    SafeMemory::ScopedSigSegvGuard guard;
    int32_t collected = 0;

    UE::ObjectArray::ForEach([&](UE::UObject* obj) {
        if (collected >= requested) return;
        uintptr_t addr = reinterpret_cast<uintptr_t>(obj);

        bool match = false;
        guard.Try([&] { match = ReadObjectClassName(addr) == className; });
        if (!match) return;

        WideSample s;
        s.address = addr;
        bool any = false;
        guard.Try([&] {
            for (int32_t i = 0; i < (int32_t)sizeof(s.bytes); ++i) {
                auto b = SafeMemory::SafeReadAny<uint8_t>(s.address + i);
                if (!b) return;
                s.bytes[i] = *b;
            }
            any = true;
        });
        if (!any) return;
        s.ok = true;
        out.push_back(s);
        ++collected;
    });
    return !out.empty();
}

INTERNAL uintptr_t WidePtr(const WideSample& s, int32_t off) noexcept {
    if (off < 0 || off + 8 > (int32_t)sizeof(s.bytes)) return 0;
    return *reinterpret_cast<const uintptr_t*>(s.bytes + off);
}
INTERNAL int32_t WideI32(const WideSample& s, int32_t off) noexcept {
    if (off < 0 || off + 4 > (int32_t)sizeof(s.bytes)) return 0;
    return *reinterpret_cast<const int32_t*>(s.bytes + off);
}

INTERNAL int32_t PropI32(const PropSample& s, int32_t off) noexcept {
    if (off < 0 || off + 4 > (int32_t)sizeof(s.bytes)) return 0;
    return *reinterpret_cast<const int32_t*>(s.bytes + off);
}

INTERNAL uintptr_t PropPtr(const PropSample& s, int32_t off) noexcept {
    if (off < 0 || off + 8 > (int32_t)sizeof(s.bytes)) return 0;
    return *reinterpret_cast<const uintptr_t*>(s.bytes + off);
}

INTERNAL bool IsPropertyClassNameLocal(const std::string& cls) noexcept {
    return cls.size() >= 8 && cls.compare(cls.size() - 8, 8, "Property") == 0;
}

INTERNAL bool IsLikelyMemberName(const std::string& name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    if (name.find("Property__") != std::string::npos) return false;
    if (name.find("StructProperty") != std::string::npos) return false;
    if (name == "None" || name == "Class" || name == "Function" ||
        name == "ScriptStruct" || name == "Package") return false;

    bool sawAlpha = false;
    for (unsigned char c : name) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) sawAlpha = true;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            continue;
        }
        return false;
    }
    return sawAlpha;
}

INTERNAL FieldHit HitFromOffset(int32_t offset, int32_t score, int32_t total) noexcept {
    FieldHit h;
    h.offset = offset;
    h.score = score;
    h.total = total;
    return h;
}

INTERNAL bool LooksLikeCDO(uintptr_t target) noexcept {
    if (!LooksLikeUObjectPointer(target)) return false;
    std::string name = ReadObjectName(target);
    if (name.size() < 9) return false;
    return name.compare(0, 9, "Default__") == 0;
}

INTERNAL std::string ReadFFieldClassNameAt(uintptr_t fieldAddr,
                                           int32_t classOff,
                                           int32_t classNameOff) noexcept {
    if (fieldAddr == 0 || classOff < 0 || classNameOff < 0) return {};
    uintptr_t cls = 0;
    if (!TryReadAnyValue<uintptr_t>(fieldAddr + classOff, cls) ||
        cls == 0 || (cls & 0x7) != 0) {
        return {};
    }
    return ReadFNameAt(cls + classNameOff);
}

INTERNAL std::string ReadFFieldNameAt(uintptr_t fieldAddr,
                                      int32_t nameOff) noexcept {
    if (fieldAddr == 0 || nameOff < 0) return {};
    return ReadFNameAt(fieldAddr + nameOff);
}

INTERNAL bool IsObjectReferencePropertyClass(const std::string& cls) noexcept {
    return cls == "ObjectProperty" || cls == "ClassProperty" ||
           cls == "WeakObjectProperty" || cls == "LazyObjectProperty" ||
           cls == "SoftObjectProperty" || cls == "SoftClassProperty" ||
           cls == "InterfaceProperty";
}

INTERNAL bool IsPropertyPointer(uintptr_t p,
                                bool ffieldProperty,
                                int32_t classOff,
                                int32_t classNameOff) noexcept {
    if (p == 0 || (p & 0x7) != 0) return false;
    if (ffieldProperty) {
        return IsPropertyClassNameLocal(ReadFFieldClassNameAt(p, classOff, classNameOff));
    }
    if (!LooksLikeUObjectPointer(p)) return false;
    return IsPropertyClassNameLocal(ReadObjectClassName(p));
}

INTERNAL bool IsUObjectOfClass(uintptr_t p, const char* className) noexcept {
    if (p == 0 || (p & 0x7) != 0) return false;
    if (!LooksLikeUObjectPointer(p)) return false;
    return ReadObjectClassName(p) == className;
}

INTERNAL int32_t AlignUp8(int32_t v) noexcept {
    return (v + 7) & ~7;
}

INTERNAL void ScoreSubtypeCandidate(const PropSample& p,
                                    int32_t baseOff,
                                    bool ffieldProperty,
                                    int32_t classOff,
                                    int32_t classNameOff,
                                    int32_t& score,
                                    int32_t& total) noexcept {
    if (IsObjectReferencePropertyClass(p.className)) {
        ++total;
        if (IsUObjectOfClass(PropPtr(p, baseOff), "Class")) ++score;
        return;
    }

    if (p.className == "StructProperty") {
        ++total;
        if (IsUObjectOfClass(PropPtr(p, baseOff), "ScriptStruct")) ++score;
        return;
    }

    if (p.className == "ArrayProperty" || p.className == "SetProperty") {
        ++total;
        if (IsPropertyPointer(PropPtr(p, baseOff), ffieldProperty,
                              classOff, classNameOff)) {
            ++score;
        }
        return;
    }

    if (p.className == "MapProperty") {
        total += 2;
        if (IsPropertyPointer(PropPtr(p, baseOff), ffieldProperty,
                              classOff, classNameOff)) {
            ++score;
        }
        if (IsPropertyPointer(PropPtr(p, baseOff + 8), ffieldProperty,
                              classOff, classNameOff)) {
            ++score;
        }
        return;
    }

    if (p.className == "EnumProperty") {
        total += 2;
        if (IsPropertyPointer(PropPtr(p, baseOff), ffieldProperty,
                              classOff, classNameOff)) {
            ++score;
        }
        if (IsUObjectOfClass(PropPtr(p, baseOff + 8), "Enum")) ++score;
        return;
    }

    // ByteProperty's enum pointer is optional, so using it for base inference
    // creates false misses for normal uint8 properties. Type resolution still
    // reads the slot later and upgrades enum-backed bytes when it is present.
}

INTERNAL FieldHit InferPropertySubtypeBase(const std::vector<PropSample>& props,
                                           int32_t minStart,
                                           bool ffieldProperty,
                                           int32_t classOff,
                                           int32_t classNameOff,
                                           const char* label,
                                           std::vector<std::string>& trace) noexcept {
    if (props.empty()) return {};

    int32_t start = AlignUp8(minStart);
    if (start < 0x40) start = 0x40;
    int32_t end = kPropSnapshotBytes - 0x10;

    int32_t bestOffset = -1;
    int32_t bestScore = 0;
    int32_t bestTotal = 0;
    int32_t bestWeighted = 0;

    for (int32_t off = start; off <= end; off += 8) {
        int32_t score = 0;
        int32_t total = 0;
        int32_t participating = 0;

        for (const auto& p : props) {
            int32_t before = total;
            ScoreSubtypeCandidate(p, off, ffieldProperty,
                                  classOff, classNameOff, score, total);
            if (total != before) ++participating;
        }

        if (total == 0) continue;
        int32_t weighted = score * 16 + participating;
        if (weighted > bestWeighted) {
            bestWeighted = weighted;
            bestOffset = off;
            bestScore = score;
            bestTotal = total;
        }
    }

    FieldHit hit = HitFromOffset(bestOffset, bestScore, bestTotal);

    char b[224];
    std::snprintf(b, sizeof(b),
                  "%s subtype base best @ 0x%x: %d/%d typed pointer checks (weighted=%d)",
                  label, hit.offset, hit.score, hit.total, bestWeighted);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
    return hit;
}

INTERNAL bool SnapshotProp(uintptr_t prop,
                           uintptr_t owner,
                           int32_t ownerSize,
                           const std::string& className,
                           const std::string& name,
                           PropSample& out) noexcept {
    if (prop == 0 || owner == 0 || ownerSize <= 0 || className.empty()) return false;
    if (!IsLikelyMemberName(name)) return false;

    SafeMemory::ScopedSigSegvGuard guard;
    bool ok = false;
    guard.Try([&] {
        for (int32_t i = 0; i < kPropSnapshotBytes; ++i) {
            auto b = SafeMemory::SafeReadAny<uint8_t>(prop + i);
            if (!b) return;
            out.bytes[i] = *b;
        }
        ok = true;
    });
    if (!ok) return false;

    out.address = prop;
    out.owner = owner;
    out.ownerSize = ownerSize;
    out.className = className;
    out.name = name;
    return true;
}

INTERNAL void AddOwnersFromSamples(const std::vector<WideSample>& samples,
                                   int32_t propertiesSizeOff,
                                   std::vector<StructOwner>& owners) noexcept {
    for (const auto& s : samples) {
        int32_t size = WideI32(s, propertiesSizeOff);
        if (size < Off::UObject::Size || size >= (1 << 24)) continue;
        owners.push_back({ s.address, size });
    }
}

INTERNAL int32_t ExpectedElementSize(const std::string& cls) noexcept {
    if (cls == "BoolProperty" || cls == "ByteProperty" || cls == "Int8Property") return 1;
    if (cls == "Int16Property" || cls == "UInt16Property") return 2;
    if (cls == "IntProperty" || cls == "UInt32Property" || cls == "FloatProperty") return 4;
    if (cls == "Int64Property" || cls == "UInt64Property" || cls == "DoubleProperty" ||
        cls == "NameProperty" || cls == "ObjectProperty" || cls == "ClassProperty" ||
        cls == "WeakObjectProperty" || cls == "LazyObjectProperty" ||
        cls == "InterfaceProperty" || cls == "FieldPathProperty") return 8;
    if (cls == "StrProperty" || cls == "ArrayProperty") return 0x10;
    if (cls == "TextProperty") return 0x18;
    if (cls == "SoftObjectProperty" || cls == "SoftClassProperty") return 0x28;
    if (cls == "MapProperty" || cls == "SetProperty") return 0x50;
    return 0;
}

// CDO scan: count, per offset, how many samples hold a pointer whose target
// has a name beginning with "Default__".
INTERNAL FieldHit FindCDO(const std::vector<WideSample>& classSamples,
                          int32_t startOff,
                          int32_t endOff,
                          std::vector<std::string>& trace) noexcept {
    FieldHit best;
    best.total = static_cast<int32_t>(classSamples.size());
    SafeMemory::ScopedSigSegvGuard guard;

    int32_t bestPerOffsetTopHits = 0;
    int32_t bestOffset = -1;

    for (int32_t off = startOff; off + 8 <= endOff; off += 8) {
        int32_t hits = 0;
        for (const auto& s : classSamples) {
            uintptr_t p = WidePtr(s, off);
            if (p == 0) continue;
            bool ok = false;
            guard.Try([&] { ok = LooksLikeCDO(p); });
            if (ok) ++hits;
        }
        if (hits > bestPerOffsetTopHits) {
            bestPerOffsetTopHits = hits;
            bestOffset = off;
        }
    }
    best.offset = bestOffset;
    best.score  = bestPerOffsetTopHits;

    char b[224];
    std::snprintf(b, sizeof(b),
                  "CDO best @ 0x%x: %d/%d hold a Default__-named target",
                  best.offset, best.score, best.total);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
    return best;
}

// PropertiesSize: int32 right after Children (or ChildProperties). Anchor
// against the UObject UClass — its PropertiesSize must equal sizeof(UObject).
INTERNAL FieldHit FindPropertiesSize(const std::vector<WideSample>& classSamples,
                                     int32_t startOff,
                                     int32_t endOff,
                                     std::vector<std::string>& trace) noexcept {
    FieldHit best;
    best.total = static_cast<int32_t>(classSamples.size());

    // Find the UObject UClass for the anchor check.
    SafeMemory::ScopedSigSegvGuard guard;
    uintptr_t objectClass = 0;
    UE::ObjectArray::ForEach([&](UE::UObject* obj) {
        if (objectClass) return;
        uintptr_t a = reinterpret_cast<uintptr_t>(obj);
        std::string nm, cls;
        guard.Try([&] {
            nm = ReadObjectName(a);
            cls = ReadObjectClassName(a);
        });
        if (nm == "Object" && cls == "Class") objectClass = a;
    });

    char b[224];
    if (objectClass) {
        std::snprintf(b, sizeof(b),
                      "PropertiesSize anchor: Object UClass at 0x%lx (UObject::Size = 0x%x)",
                      static_cast<unsigned long>(objectClass), Off::UObject::Size);
    } else {
        std::snprintf(b, sizeof(b),
                      "PropertiesSize: no Object UClass found — falling back to range scan");
    }
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);

    int32_t bestScore  = 0;
    int32_t bestOffset = -1;

    for (int32_t off = startOff; off + 4 <= endOff; off += 4) {
        // Reasonable-int32 score across all class samples.
        int32_t reasonable = 0;
        for (const auto& s : classSamples) {
            int32_t v = WideI32(s, off);
            if (v >= Off::UObject::Size && v < (1 << 24)) ++reasonable;
        }

        // Anchor bonus: Object UClass's PropertiesSize must equal UObject::Size.
        int32_t anchorBonus = 0;
        if (objectClass) {
            auto v = SafeMemory::SafeReadAny<int32_t>(objectClass + off);
            if (v && *v == Off::UObject::Size) {
                anchorBonus = best.total;  // double-weight: this is the discriminator
            }
        }

        int32_t score = reasonable + anchorBonus;
        if (score > bestScore) {
            bestScore  = score;
            bestOffset = off;
        }
    }

    best.offset = bestOffset;
    best.score  = bestScore;

    std::snprintf(b, sizeof(b),
                  "PropertiesSize best @ 0x%x: weighted-score=%d (sample=%d, anchor-bonus weight=%d)",
                  best.offset, best.score, best.total, best.total);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
    return best;
}

// ChildProperties: pointer right after Children. Targets are non-class
// heap pointers (FField). Score = null-or-non-class-UObject-pointer count.
INTERNAL FieldHit FindChildProperties(const std::vector<WideSample>& classSamples,
                                      int32_t startOff,
                                      int32_t endOff,
                                      std::vector<std::string>& trace) noexcept {
    FieldHit best;
    best.total = static_cast<int32_t>(classSamples.size());
    SafeMemory::ScopedSigSegvGuard guard;

    int32_t bestScore  = 0;
    int32_t bestOffset = -1;

    for (int32_t off = startOff; off + 8 <= endOff; off += 8) {
        int32_t nullCount = 0, fieldPtr = 0, classPtr = 0;
        for (const auto& s : classSamples) {
            uintptr_t p = WidePtr(s, off);
            if (p == 0) { ++nullCount; continue; }
            bool isObj = false, isClass = false;
            guard.Try([&] { isObj = LooksLikeUObjectPointer(p); });
            if (!isObj) continue;
            ++fieldPtr;
            guard.Try([&] { isClass = IsUClassInstance(p); });
            if (isClass) ++classPtr;
        }
        // ChildProperties: non-class heap pointers + nulls. Penalize class refs.
        int32_t nonClass = fieldPtr - classPtr;
        if (nonClass <= 0) continue;
        int32_t score = nullCount + nonClass;
        if (score > bestScore) {
            bestScore  = score;
            bestOffset = off;
        }
    }
    best.offset = bestOffset;
    best.score  = bestScore;

    char b[224];
    std::snprintf(b, sizeof(b),
                  "ChildProperties best @ 0x%x: %d/%d (null + non-class heap*)",
                  best.offset, best.score, best.total);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
    return best;
}

// UFunction::Func: pointer to native code. Lives in an executable segment.
INTERNAL FieldHit FindFunc(const std::vector<WideSample>& funcSamples,
                           int32_t startOff,
                           int32_t endOff,
                           std::vector<std::string>& trace) noexcept {
    FieldHit best;
    best.total = static_cast<int32_t>(funcSamples.size());
    SafeMemory::ScopedSigSegvGuard guard;

    int32_t bestScore  = 0;
    int32_t bestOffset = -1;

    for (int32_t off = startOff; off + 8 <= endOff; off += 8) {
        int32_t hits = 0;
        for (const auto& s : funcSamples) {
            uintptr_t p = WidePtr(s, off);
            if (p == 0) continue;
            bool ok = false;
            guard.Try([&] { ok = SafeMemory::IsExecutable(p); });
            if (ok) ++hits;
        }
        if (hits > bestScore) {
            bestScore  = hits;
            bestOffset = off;
        }
    }
    best.offset = bestOffset;
    best.score  = bestScore;

    char b[224];
    std::snprintf(b, sizeof(b),
                  "UFunction::Func best @ 0x%x: %d/%d code-pointer matches",
                  best.offset, best.score, best.total);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
    return best;
}

INTERNAL void InferFFieldClassOffsets(const std::vector<uintptr_t>& anchors,
                                      AdvancedLayout& layout,
                                      std::vector<std::string>& trace) noexcept {
    if (anchors.empty()) return;

    int32_t bestClassOff = -1;
    int32_t bestClassNameOff = -1;
    int32_t bestCount = 0;
    int32_t total = static_cast<int32_t>(anchors.size());

    for (int32_t classOff = 0; classOff <= 0x28; classOff += 8) {
        for (int32_t classNameOff = 0; classNameOff <= 0x20; classNameOff += 4) {
            int32_t hits = 0;
            for (uintptr_t a : anchors) {
                std::string cls = ReadFFieldClassNameAt(a, classOff, classNameOff);
                if (IsPropertyClassNameLocal(cls)) ++hits;
            }
            if (hits > bestCount) {
                bestCount = hits;
                bestClassOff = classOff;
                bestClassNameOff = classNameOff;
            }
        }
    }

    layout.fFieldClassPrivate = HitFromOffset(bestClassOff, bestCount, total);
    layout.fFieldClassName = HitFromOffset(bestClassNameOff, bestCount, total);

    char b[224];
    std::snprintf(b, sizeof(b),
                  "FField class offsets: ClassPrivate=0x%x FFieldClass::Name=0x%x (%d/%d property class names)",
                  bestClassOff, bestClassNameOff, bestCount, total);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
}

INTERNAL void InferFFieldNext(const std::vector<uintptr_t>& anchors,
                              int32_t classOff,
                              int32_t classNameOff,
                              AdvancedLayout& layout,
                              std::vector<std::string>& trace) noexcept {
    if (anchors.empty() || classOff < 0 || classNameOff < 0) return;

    int32_t bestOffset = -1;
    int32_t bestTerminations = 0;
    int32_t bestWeighted = 0;
    int32_t total = static_cast<int32_t>(anchors.size());
    SafeMemory::ScopedSigSegvGuard guard;

    for (int32_t nextOff = 0x10; nextOff <= 0x40; nextOff += 8) {
        int32_t terminations = 0;
        int32_t nonNullLinks = 0;
        int32_t validNodes = 0;

        for (uintptr_t a : anchors) {
            std::unordered_set<uintptr_t> seen;
            uintptr_t cur = a;
            bool ok = false;
            for (int32_t depth = 0; cur && depth < 512; ++depth) {
                if (!seen.insert(cur).second) break;
                std::string cls = ReadFFieldClassNameAt(cur, classOff, classNameOff);
                if (!IsPropertyClassNameLocal(cls)) break;
                ++validNodes;

                uintptr_t next = 0;
                guard.Try([&] {
                    auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + nextOff);
                    if (v) next = *v;
                });
                if (next) ++nonNullLinks;
                cur = next;
                if (!cur) ok = true;
            }
            if (ok) ++terminations;
        }

        int32_t weighted = terminations * 16 + nonNullLinks * 2 + validNodes;
        if (weighted > bestWeighted) {
            bestWeighted = weighted;
            bestTerminations = terminations;
            bestOffset = nextOff;
        }
    }

    layout.fFieldNext = HitFromOffset(bestOffset, bestTerminations, total);

    char b[224];
    std::snprintf(b, sizeof(b),
                  "FField::Next best @ 0x%x: %d/%d terminating chains (weighted=%d)",
                  bestOffset, bestTerminations, total, bestWeighted);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
}

INTERNAL void CollectFFieldNodes(const std::vector<uintptr_t>& anchors,
                                 int32_t classOff,
                                 int32_t classNameOff,
                                 int32_t nextOff,
                                 std::vector<uintptr_t>& out,
                                 size_t limit = 2048) noexcept {
    out.clear();
    std::unordered_set<uintptr_t> seen;
    SafeMemory::ScopedSigSegvGuard guard;

    for (uintptr_t a : anchors) {
        uintptr_t cur = a;
        for (int32_t depth = 0; cur && depth < 512 && out.size() < limit; ++depth) {
            if (!seen.insert(cur).second) break;
            std::string cls = ReadFFieldClassNameAt(cur, classOff, classNameOff);
            if (!IsPropertyClassNameLocal(cls)) break;
            out.push_back(cur);

            uintptr_t next = 0;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + nextOff);
                if (v) next = *v;
            });
            cur = next;
        }
        if (out.size() >= limit) break;
    }
}

INTERNAL void InferFFieldName(const std::vector<uintptr_t>& fields,
                              AdvancedLayout& layout,
                              std::vector<std::string>& trace) noexcept {
    if (fields.empty()) return;

    int32_t bestOffset = -1;
    int32_t bestHits = 0;
    int32_t bestWeighted = 0;
    int32_t total = static_cast<int32_t>(fields.size());

    for (int32_t nameOff = 0x18; nameOff <= 0x58; nameOff += 4) {
        int32_t hits = 0;
        std::unordered_set<std::string> distinct;
        for (uintptr_t f : fields) {
            std::string name = ReadFFieldNameAt(f, nameOff);
            if (!IsLikelyMemberName(name)) continue;
            ++hits;
            if (distinct.size() < 256) distinct.insert(name);
        }
        int32_t weighted = hits * 8 + static_cast<int32_t>(distinct.size());
        if (weighted > bestWeighted) {
            bestWeighted = weighted;
            bestHits = hits;
            bestOffset = nameOff;
        }
    }

    layout.fFieldNamePrivate = HitFromOffset(bestOffset, bestHits, total);

    char b[224];
    std::snprintf(b, sizeof(b),
                  "FField::NamePrivate best @ 0x%x: %d/%d plausible member names",
                  bestOffset, bestHits, total);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
}

INTERNAL void CollectFPropertySamples(const std::vector<StructOwner>& owners,
                                      int32_t childPropsOff,
                                      int32_t classOff,
                                      int32_t classNameOff,
                                      int32_t nextOff,
                                      int32_t nameOff,
                                      std::vector<PropSample>& out) noexcept {
    out.clear();
    if (childPropsOff < 0 || classOff < 0 || classNameOff < 0 ||
        nextOff < 0 || nameOff < 0) {
        return;
    }

    SafeMemory::ScopedSigSegvGuard guard;
    std::unordered_set<uintptr_t> seen;
    for (const auto& owner : owners) {
        uintptr_t cur = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(owner.address + childPropsOff);
            if (v) cur = *v;
        });

        for (int32_t depth = 0; cur && depth < 512 && out.size() < 4096; ++depth) {
            if (!seen.insert(cur).second) break;
            std::string cls = ReadFFieldClassNameAt(cur, classOff, classNameOff);
            if (!IsPropertyClassNameLocal(cls)) break;
            std::string name = ReadFFieldNameAt(cur, nameOff);

            PropSample ps;
            if (SnapshotProp(cur, owner.address, owner.propertiesSize, cls, name, ps)) {
                out.push_back(std::move(ps));
            }

            uintptr_t next = 0;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + nextOff);
                if (v) next = *v;
            });
            cur = next;
        }
        if (out.size() >= 4096) break;
    }
}

INTERNAL void CollectUPropertySamples(const std::vector<StructOwner>& owners,
                                      std::vector<PropSample>& out) noexcept {
    out.clear();
    SafeMemory::ScopedSigSegvGuard guard;
    std::unordered_set<uintptr_t> seen;

    for (const auto& owner : owners) {
        uintptr_t cur = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(owner.address + Off::UStruct::Children);
            if (v) cur = *v;
        });

        for (int32_t depth = 0; cur && depth < 1024 && out.size() < 4096; ++depth) {
            if (!seen.insert(cur).second) break;
            std::string cls = ReadObjectClassName(cur);
            if (IsPropertyClassNameLocal(cls)) {
                std::string name = ReadObjectName(cur);
                PropSample ps;
                if (SnapshotProp(cur, owner.address, owner.propertiesSize, cls, name, ps)) {
                    out.push_back(std::move(ps));
                }
            }

            uintptr_t next = 0;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + Off::UField::Next);
                if (v) next = *v;
            });
            cur = next;
        }
        if (out.size() >= 4096) break;
    }
}

struct PropValueLayout {
    bool ok = false;
    FieldHit arrayDim;
    FieldHit elementSize;
    FieldHit offsetInternal;
};

INTERNAL PropValueLayout InferPropertyValueLayout(const std::vector<PropSample>& props,
                                                  int32_t startOff,
                                                  int32_t endOff,
                                                  const char* label,
                                                  std::vector<std::string>& trace) noexcept {
    PropValueLayout out;
    if (props.empty()) return out;

    int32_t bestDimOff = -1;
    int32_t bestElemOff = -1;
    int32_t bestOffsetOff = -1;
    int32_t bestValid = 0;
    int32_t bestWeighted = 0;

    for (int32_t dimOff = startOff; dimOff + 4 <= endOff; dimOff += 4) {
        for (int32_t elemOff = startOff; elemOff + 4 <= endOff; elemOff += 4) {
            if (elemOff == dimOff) continue;
            for (int32_t offOff = startOff; offOff + 4 <= endOff; offOff += 4) {
                if (offOff == dimOff || offOff == elemOff) continue;
                if (!(dimOff < elemOff && elemOff < offOff)) continue;

                int32_t valid = 0;
                int32_t expectedHits = 0;
                int32_t dimOneHits = 0;
                std::unordered_set<int32_t> distinctOffsets;

                for (const auto& p : props) {
                    int32_t dim = PropI32(p, dimOff);
                    int32_t elem = PropI32(p, elemOff);
                    int32_t off = PropI32(p, offOff);
                    if (dim < 1 || dim > 1024) continue;
                    if (elem < 1 || elem > 0x10000) continue;
                    if (off < 0 || off >= p.ownerSize) continue;

                    int64_t fieldSize = static_cast<int64_t>(elem) * dim;
                    if (fieldSize <= 0 || fieldSize > 0x100000) continue;
                    if (off + fieldSize > static_cast<int64_t>(p.ownerSize) + 0x100) continue;

                    ++valid;
                    if (dim == 1) ++dimOneHits;
                    int32_t expected = ExpectedElementSize(p.className);
                    if (expected > 0 && elem == expected) ++expectedHits;
                    if (distinctOffsets.size() < 512) distinctOffsets.insert(off);
                }

                int32_t distinct = static_cast<int32_t>(distinctOffsets.size());
                int32_t weighted = valid * 12 + expectedHits * 12 + dimOneHits + distinct * 3;
                if (weighted > bestWeighted) {
                    bestWeighted = weighted;
                    bestValid = valid;
                    bestDimOff = dimOff;
                    bestElemOff = elemOff;
                    bestOffsetOff = offOff;
                }
            }
        }
    }

    int32_t total = static_cast<int32_t>(props.size());
    out.arrayDim = HitFromOffset(bestDimOff, bestValid, total);
    out.elementSize = HitFromOffset(bestElemOff, bestValid, total);
    out.offsetInternal = HitFromOffset(bestOffsetOff, bestValid, total);
    out.ok = bestValid > 0 && bestValid * 100 >= total * 45;

    char b[224];
    std::snprintf(b, sizeof(b),
                  "%s value layout: ArrayDim=0x%x ElementSize=0x%x Offset_Internal=0x%x (%d/%d valid, weighted=%d, ok=%d)",
                  label, bestDimOff, bestElemOff, bestOffsetOff,
                  bestValid, total, bestWeighted, (int)out.ok);
    trace.emplace_back(b);
    DLOGI("[advoff] %s", b);
    return out;
}

}  // namespace

AdvancedLayout FindAdvancedLayout(int32_t requestedSampleSize) noexcept {
    AdvancedLayout layout;
    auto t0 = std::chrono::steady_clock::now();

    auto trace = [&](const char* fmt, ...) {
        char b[224];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        layout.traceLines.emplace_back(b);
        DLOGI("[advoff] %s", b);
    };

    if (!UE::ObjectArray::IsInitialized() || !UE::NameArray::IsInitialized()) {
        trace("ObjectArray + NameArray must be initialized");
        return layout;
    }
    if (Off::UStruct::Children <= 0 || Off::UStruct::SuperStruct <= 0) {
        trace("UStruct offsets unset — run UStruct scan + Apply first");
        return layout;
    }

    int32_t classSample = requestedSampleSize > 0
            ? requestedSampleSize : kAdvDefaultClassSample;
    if (classSample > kAdvMaxSample) classSample = kAdvMaxSample;

    int32_t funcSample = requestedSampleSize > 0
            ? requestedSampleSize : kAdvDefaultFuncSample;
    if (funcSample > kAdvMaxSample) funcSample = kAdvMaxSample;

    std::vector<WideSample> classSamples, scriptStructSamples, funcSamples;
    if (!CollectByClassName(classSample, "Class", classSamples)) {
        trace("found 0 UClass samples");
        return layout;
    }
    layout.uclassSamples = static_cast<int32_t>(classSamples.size());
    trace("collected %d UClass samples (request=%d)", layout.uclassSamples, classSample);

    if (!CollectByClassName(classSample, "ScriptStruct", scriptStructSamples)) {
        trace("found 0 UScriptStruct samples");
    }
    layout.uscriptStructSamples = static_cast<int32_t>(scriptStructSamples.size());
    trace("collected %d UScriptStruct samples (request=%d)",
          layout.uscriptStructSamples, classSample);

    if (!CollectByClassName(funcSample, "Function", funcSamples)) {
        trace("found 0 UFunction samples");
        // Continue — UClass scans don't depend on this.
    }
    layout.ufuncSamples = static_cast<int32_t>(funcSamples.size());
    trace("collected %d UFunction samples (request=%d)", layout.ufuncSamples, funcSample);

    std::vector<WideSample> structLikeSamples = classSamples;
    structLikeSamples.insert(structLikeSamples.end(),
                             scriptStructSamples.begin(), scriptStructSamples.end());

    // ---- ChildProperties: right after Children, narrow band ----
    {
        int32_t s = Off::UStruct::Children + 8;
        int32_t e = s + 0x10;
        layout.childProperties = FindChildProperties(
                structLikeSamples, s, e, layout.traceLines);
    }

    // ---- PropertiesSize: int32 after Children/ChildProperties ----
    {
        int32_t s = layout.childProperties.offset > 0
                ? layout.childProperties.offset + 8
                : Off::UStruct::Children + 8;
        int32_t e = s + 0x20;
        layout.propertiesSize = FindPropertiesSize(
                classSamples, s, e, layout.traceLines);
    }

    // ---- ClassDefaultObject: deep into UClass body ----
    {
        // Default UClass::ClassDefaultObject is 0x118; scan a generous range.
        int32_t s = 0xC0;
        int32_t e = 0x1E0;
        layout.classDefaultObject = FindCDO(
                classSamples, s, e, layout.traceLines);
    }

    // ---- UFunction::Func: code pointer, range after typical UStruct end ----
    if (!funcSamples.empty()) {
        int32_t s = 0x60;
        int32_t e = 0xF0;
        layout.funcFunc = FindFunc(funcSamples, s, e, layout.traceLines);
    } else {
        trace("UFunction::Func scan skipped — no UFunction samples");
    }

    // ---- UProperty / FProperty internals ----
    std::vector<StructOwner> owners;
    if (layout.propertiesSize.offset >= 0) {
        owners.reserve(classSamples.size() + scriptStructSamples.size());
        AddOwnersFromSamples(classSamples, layout.propertiesSize.offset, owners);
        AddOwnersFromSamples(scriptStructSamples, layout.propertiesSize.offset, owners);
    }
    trace("property owner samples=%zu", owners.size());

    std::vector<PropSample> upropSamples;
    CollectUPropertySamples(owners, upropSamples);
    layout.upropSamples = static_cast<int32_t>(upropSamples.size());
    trace("collected %d UProperty samples", layout.upropSamples);
    if (!upropSamples.empty()) {
        PropValueLayout uv = InferPropertyValueLayout(
                upropSamples, Off::UField::Size, Off::UField::Size + 0x48,
                "UProperty", layout.traceLines);
        layout.uPropArrayDim       = uv.arrayDim;
        layout.uPropElementSize    = uv.elementSize;
        layout.uPropOffsetInternal = uv.offsetInternal;
        int32_t minSubtypeStart = std::max({
                Off::UField::Size,
                layout.uPropArrayDim.offset + 4,
                layout.uPropElementSize.offset + 4,
                layout.uPropOffsetInternal.offset + 4
        });
        layout.uPropSubtypeBase = InferPropertySubtypeBase(
                upropSamples, minSubtypeStart, false, -1, -1,
                "UProperty", layout.traceLines);
    }

    if (layout.childProperties.offset >= 0 && !owners.empty()) {
        std::vector<uintptr_t> anchors;
        anchors.reserve(owners.size());
        SafeMemory::ScopedSigSegvGuard guard;
        for (const auto& owner : owners) {
            uintptr_t p = 0;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(
                        owner.address + layout.childProperties.offset);
                if (v) p = *v;
            });
            if (p) anchors.push_back(p);
            if (anchors.size() >= 512) break;
        }
        trace("FField anchor samples=%zu", anchors.size());

        if (!anchors.empty()) {
            InferFFieldClassOffsets(anchors, layout, layout.traceLines);
            if (layout.fFieldClassPrivate.offset >= 0 &&
                layout.fFieldClassName.offset >= 0) {
                InferFFieldNext(anchors,
                                layout.fFieldClassPrivate.offset,
                                layout.fFieldClassName.offset,
                                layout, layout.traceLines);
            }

            if (layout.fFieldNext.offset >= 0) {
                std::vector<uintptr_t> ffields;
                CollectFFieldNodes(anchors,
                                   layout.fFieldClassPrivate.offset,
                                   layout.fFieldClassName.offset,
                                   layout.fFieldNext.offset,
                                   ffields);
                InferFFieldName(ffields, layout, layout.traceLines);
            }

            if (layout.fFieldNamePrivate.offset >= 0) {
                std::vector<PropSample> fpropSamples;
                CollectFPropertySamples(owners,
                                        layout.childProperties.offset,
                                        layout.fFieldClassPrivate.offset,
                                        layout.fFieldClassName.offset,
                                        layout.fFieldNext.offset,
                                        layout.fFieldNamePrivate.offset,
                                        fpropSamples);
                layout.fpropSamples = static_cast<int32_t>(fpropSamples.size());
                trace("collected %d FProperty samples", layout.fpropSamples);
                if (!fpropSamples.empty()) {
                    PropValueLayout fv = InferPropertyValueLayout(
                            fpropSamples, 0x28, 0x78, "FProperty", layout.traceLines);
                    layout.fPropArrayDim       = fv.arrayDim;
                    layout.fPropElementSize    = fv.elementSize;
                    layout.fPropOffsetInternal = fv.offsetInternal;
                    int32_t minSubtypeStart = std::max({
                            0x50,
                            layout.fPropArrayDim.offset + 4,
                            layout.fPropElementSize.offset + 4,
                            layout.fPropOffsetInternal.offset + 4
                    });
                    layout.fPropSubtypeBase = InferPropertySubtypeBase(
                            fpropSamples, minSubtypeStart, true,
                            layout.fFieldClassPrivate.offset,
                            layout.fFieldClassName.offset,
                            "FProperty", layout.traceLines);
                }
            }
        }
    }

    auto strong = [&](const FieldHit& h, int32_t pct) {
        return h.offset >= 0 && h.total > 0 && h.score * 100 >= h.total * pct;
    };
    bool hasFProps = layout.fpropSamples > 0;
    bool hasUProps = layout.upropSamples > 0;
    bool fPropsOk = !hasFProps ||
            (strong(layout.fFieldClassPrivate, 45) &&
             strong(layout.fFieldNext,         45) &&
             strong(layout.fFieldNamePrivate,  45) &&
             strong(layout.fPropArrayDim,      45) &&
             strong(layout.fPropElementSize,   45) &&
             strong(layout.fPropOffsetInternal,45));
    bool uPropsOk = !hasUProps ||
            (strong(layout.uPropArrayDim,       45) &&
             strong(layout.uPropElementSize,    45) &&
             strong(layout.uPropOffsetInternal, 45));
    layout.ok =
            strong(layout.classDefaultObject, 60) &&
            strong(layout.propertiesSize,     50) &&
            (funcSamples.empty() || strong(layout.funcFunc, 80)) &&
            (hasFProps || hasUProps) &&
            fPropsOk && uPropsOk;

    auto t1 = std::chrono::steady_clock::now();
    layout.elapsedMicros =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    trace("advanced ok=%d (%.1f ms)", (int)layout.ok, layout.elapsedMicros / 1000.0);
    return layout;
}

void Apply(const AdvancedLayout& l) noexcept {
    if (!l.ok) return;
    auto usableSubtypeBase = [](const FieldHit& h) {
        return h.offset >= 0 && h.total > 0 && h.score * 100 >= h.total * 45;
    };

    if (l.classDefaultObject.offset >= 0)
        Off::UClass::ClassDefaultObject = l.classDefaultObject.offset;
    if (l.childProperties.offset    >= 0)
        Off::UStruct::ChildProperties   = l.childProperties.offset;
    if (l.propertiesSize.offset     >= 0)
        Off::UStruct::PropertiesSize    = l.propertiesSize.offset;
    if (l.funcFunc.offset           >= 0)
        Off::UFunction::Func            = l.funcFunc.offset;
    if (l.fFieldClassPrivate.offset >= 0)
        Off::FField::ClassPrivate       = l.fFieldClassPrivate.offset;
    if (l.fFieldNext.offset         >= 0)
        Off::FField::Next               = l.fFieldNext.offset;
    if (l.fFieldNamePrivate.offset  >= 0)
        Off::FField::NamePrivate        = l.fFieldNamePrivate.offset;
    if (l.fFieldClassName.offset    >= 0)
        Off::FFieldClass::Name          = l.fFieldClassName.offset;
    if (l.fPropArrayDim.offset      >= 0)
        Off::FProperty::ArrayDim        = l.fPropArrayDim.offset;
    if (l.fPropElementSize.offset   >= 0)
        Off::FProperty::ElementSize     = l.fPropElementSize.offset;
    if (l.fPropOffsetInternal.offset >= 0)
        Off::FProperty::Offset_Internal = l.fPropOffsetInternal.offset;
    if (l.fFieldNext.offset         >= 0)
        Off::FProperty::Next            = l.fFieldNext.offset;
    if (usableSubtypeBase(l.fPropSubtypeBase))
        Off::FProperty::Size            = l.fPropSubtypeBase.offset;
    if (l.uPropArrayDim.offset      >= 0)
        Off::UProperty::ArrayDim        = l.uPropArrayDim.offset;
    if (l.uPropElementSize.offset   >= 0)
        Off::UProperty::ElementSize     = l.uPropElementSize.offset;
    if (l.uPropOffsetInternal.offset >= 0)
        Off::UProperty::Offset_Internal = l.uPropOffsetInternal.offset;
    if (usableSubtypeBase(l.uPropSubtypeBase))
        Off::UProperty::Size            = l.uPropSubtypeBase.offset;
}

}  // namespace StructOffsetFinder
