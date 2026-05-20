// =============================================================================
// Core/ObjectArray.cpp
// -----------------------------------------------------------------------------
// Implementation of UE::ObjectArray. The interesting part is Init() — it
// performs three independent auto-detections from a single input address:
//
//   1. Outer wrapper detection: is `addr` pointing at GUObjectArray
//      (FUObjectArray, with the inner TUObjectArray at +0x10) or directly at
//      the inner TUObjectArray? We try inner-first; if that fails we try
//      addr+ObjObjects.
//
//   2. Chunked vs flat: read NumElements/MaxElements at both candidate
//      offsets and accept the layout that produces sane counts and a
//      readable Objects pointer.
//
//   3. FUObjectItem stride (0x18 / 0x20): read the first two items under
//      both stride hypotheses; accept the one whose item[i].Object resolves
//      to a UObject whose InternalIndex == i.
//
// Everything stays defensive: reads are bounded, optional, and the array
// walk paths are wrapped in ScopedSigSegvGuard at the call site (ForEach in
// the header, GetByIndex when called directly).
// =============================================================================

#include "ObjectArray.h"

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)
#define DLOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)
#define DLOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace UE {

namespace {

// --- module state ---------------------------------------------------------

struct State {
    bool        initialized = false;
    bool        chunked     = true;
    int32_t     itemSize    = 0x18;
    int32_t     numElements = 0;
    uintptr_t   inner       = 0;     // address of TUObjectArray
    uintptr_t   source      = 0;     // address passed to Init
    uintptr_t   objectsPtr  = 0;     // FUObjectItem** (chunked) or FUObjectItem* (flat)
    int32_t     elementsPerChunk = Off::FChunkedFixedUObjectArray::ElementsPerChunk;
    ObjectArray::DecryptFn decrypt;
};

INTERNAL State& S() noexcept {
    static State s;
    return s;
}

// --- sanity heuristics ----------------------------------------------------

constexpr int32_t kMinSaneObjectCount = 1024;       // any real UE game has > 1k UObjects
constexpr int32_t kMaxSaneObjectCount = 100'000'000;
constexpr int32_t kMaxSaneChunkCount  = 4096;

INTERNAL bool LooksSaneCount(int32_t n) noexcept {
    return n >= kMinSaneObjectCount && n <= kMaxSaneObjectCount;
}

// Read a uint32_t / pointer / etc. from a libUE4 address (preferred) but fall
// back to a relaxed read if the address isn't covered by the libUE4 segment
// cache (happens when callers pass an inner array that lives outside libUE4).
template <typename T>
INTERNAL std::optional<T> ReadStruct(uintptr_t addr) noexcept {
    auto strict = SafeMemory::SafeRead<T>(addr);
    if (strict.has_value()) return strict;
    return SafeMemory::SafeReadAny<T>(addr);
}

INTERNAL bool LooksLikeChunked(uintptr_t base) noexcept {
    auto numEl  = ReadStruct<int32_t>(base + Off::FChunkedFixedUObjectArray::NumElements);
    auto maxEl  = ReadStruct<int32_t>(base + Off::FChunkedFixedUObjectArray::MaxElements);
    auto numCh  = ReadStruct<int32_t>(base + Off::FChunkedFixedUObjectArray::NumChunks);
    auto maxCh  = ReadStruct<int32_t>(base + Off::FChunkedFixedUObjectArray::MaxChunks);
    auto chunks = ReadStruct<uintptr_t>(base + Off::FChunkedFixedUObjectArray::Objects);

    if (!numEl || !maxEl || !numCh || !maxCh || !chunks) return false;
    if (!LooksSaneCount(*numEl)) return false;
    if (*maxEl < *numEl) return false;
    if (*numCh <= 0 || *numCh > kMaxSaneChunkCount) return false;
    if (*maxCh < *numCh) return false;
    if (*chunks == 0) return false;
    return true;
}

INTERNAL bool LooksLikeFlat(uintptr_t base) noexcept {
    auto numEl  = ReadStruct<int32_t>(base + Off::FFixedUObjectArray::NumElements);
    auto maxEl  = ReadStruct<int32_t>(base + Off::FFixedUObjectArray::MaxElements);
    auto items  = ReadStruct<uintptr_t>(base + Off::FFixedUObjectArray::Objects);

    if (!numEl || !maxEl || !items) return false;
    if (!LooksSaneCount(*numEl)) return false;
    if (*maxEl < *numEl) return false;
    if (*items == 0) return false;
    return true;
}

// --- decryption ----------------------------------------------------------

INTERNAL UObject* Decrypt(void* raw) noexcept {
    State& s = S();
    if (!s.decrypt) return reinterpret_cast<UObject*>(raw);
    return reinterpret_cast<UObject*>(s.decrypt(raw));
}

// --- item-stride probe ----------------------------------------------------
//
// Slot of FUObjectItem[i] under stride:
//   chunked: item = chunkPtr[i / EPC] + (i % EPC) * stride
//   flat   : item = base                + i        * stride
// We resolve item[0].Object and item[1].Object under each stride hypothesis,
// then read each Object->InternalIndex (UObject + 0x0C). The correct stride
// is the one where InternalIndex == i for both samples.

INTERNAL uintptr_t ItemAddrChunked(uintptr_t chunksBase, int32_t i,
                                   int32_t stride, int32_t epc) noexcept {
    auto chunkPtr = ReadStruct<uintptr_t>(chunksBase + (i / epc) * sizeof(uintptr_t));
    if (!chunkPtr || *chunkPtr == 0) return 0;
    return *chunkPtr + (i % epc) * stride;
}

INTERNAL uintptr_t ItemAddrFlat(uintptr_t itemsBase, int32_t i, int32_t stride) noexcept {
    return itemsBase + i * stride;
}

INTERNAL bool ProbeStride(bool chunked, uintptr_t objectsPtr, int32_t epc,
                          int32_t stride, int32_t numElements) noexcept {
    if (numElements < 2) return false;

    auto itemAddr = [&](int32_t i) -> uintptr_t {
        return chunked ? ItemAddrChunked(objectsPtr, i, stride, epc)
                       : ItemAddrFlat   (objectsPtr, i, stride);
    };

    const int32_t sampleLimit = numElements < 512 ? numElements : 512;
    int32_t matches = 0;

    for (int32_t i = 0; i < sampleLimit; ++i) {
        uintptr_t ia = itemAddr(i);
        if (ia == 0) continue;
        auto rawObj = SafeMemory::SafeReadAny<void*>(ia);
        if (!rawObj || *rawObj == nullptr) continue;
        UObject* obj = Decrypt(*rawObj);
        if (obj == nullptr) continue;

        // InternalIndex must equal the slot index.
        auto idx = SafeMemory::SafeReadAny<int32_t>(
                reinterpret_cast<uintptr_t>(obj) + Off::UObject::InternalIndex);
        if (!idx || *idx != i) continue;

        // VTable must point into libUE4 — anywhere readable. Includes
        // .text, .rodata, AND the writable PT_LOAD that hosts .data.rel.ro
        // (where modern toolchains put C++ vtables; mprotect'd read-only
        // at runtime via PT_GNU_RELRO). Filtering on PF_W misses the RELRO
        // case entirely, which is what broke Symbol on monolithic UE4
        // builds — going back to the original IsInLibUE4 strict check.
        auto vtable = SafeMemory::SafeReadAny<uintptr_t>(
                reinterpret_cast<uintptr_t>(obj) + Off::UObject::VTable);
        if (!vtable || *vtable == 0) continue;
        if (!SafeMemory::IsInLibUE4(*vtable, sizeof(uintptr_t))) continue;

        if (++matches >= 2) return true;
    }
    return false;
}

INTERNAL int32_t DetectItemSize(bool chunked, uintptr_t objectsPtr,
                                int32_t epc, int32_t numElements) noexcept {
    if (ProbeStride(chunked, objectsPtr, epc, 0x18, numElements)) return 0x18;
    if (ProbeStride(chunked, objectsPtr, epc, 0x20, numElements)) return 0x20;
    return 0;
}

// --- single-candidate Init ------------------------------------------------

struct Candidate {
    bool      ok          = false;
    bool      chunked     = false;
    int32_t   numElements = 0;
    int32_t   itemSize    = 0;
    uintptr_t objectsPtr  = 0;
    int32_t   epc         = Off::FChunkedFixedUObjectArray::ElementsPerChunk;
};

INTERNAL Candidate TryAt(uintptr_t base) noexcept {
    Candidate c;

    if (LooksLikeChunked(base)) {
        c.chunked     = true;
        c.numElements = ReadStruct<int32_t>(base + Off::FChunkedFixedUObjectArray::NumElements).value_or(0);
        c.objectsPtr  = ReadStruct<uintptr_t>(base + Off::FChunkedFixedUObjectArray::Objects).value_or(0);
        c.epc         = Off::FChunkedFixedUObjectArray::ElementsPerChunk;
    } else if (LooksLikeFlat(base)) {
        c.chunked     = false;
        c.numElements = ReadStruct<int32_t>(base + Off::FFixedUObjectArray::NumElements).value_or(0);
        c.objectsPtr  = ReadStruct<uintptr_t>(base + Off::FFixedUObjectArray::Objects).value_or(0);
    } else {
        return c;
    }

    if (c.objectsPtr == 0 || c.numElements <= 0) return c;

    // Caller is responsible for installing a ScopedSigSegvGuard around this
    // call — DetectItemSize dereferences chunk pointers that may be invalid.
    int32_t stride = DetectItemSize(c.chunked, c.objectsPtr, c.epc, c.numElements);
    if (stride == 0) return c;

    c.itemSize = stride;
    c.ok       = true;
    return c;
}

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool ObjectArray::Init(uintptr_t addr) noexcept {
    State& s = S();
    s.initialized = false;
    s.source      = addr;

    if (addr == 0) {
        DLOGE("ObjectArray::Init: null address");
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;

    Candidate c;
    uintptr_t innerAddr = addr;
    guard.Try([&] { c = TryAt(addr); });

    if (!c.ok) {
        const uintptr_t alt = addr + Off::FUObjectArray::ObjObjects;
        Candidate c2;
        guard.Try([&] { c2 = TryAt(alt); });
        if (c2.ok) {
            c         = c2;
            innerAddr = alt;
            DLOGI("ObjectArray::Init: source treated as FUObjectArray; inner @ +0x%x",
                  Off::FUObjectArray::ObjObjects);
        }
    }

    if (!c.ok) {
        DLOGE("ObjectArray::Init: layout detection failed at 0x%lx",
              static_cast<unsigned long>(addr));
        return false;
    }

    s.chunked          = c.chunked;
    s.itemSize         = c.itemSize;
    s.numElements      = c.numElements;
    s.objectsPtr       = c.objectsPtr;
    s.elementsPerChunk = c.epc;
    s.inner            = innerAddr;
    s.initialized      = true;

    Off::InSDK::bIsChunked       = c.chunked;
    Off::InSDK::FUObjectItemSize = c.itemSize;
    Off::InSDK::GObjectsObjObjectsOffset = static_cast<int32_t>(innerAddr - addr);
    Off::Resolved::GObjects      = addr;

    DLOGI("ObjectArray::Init: %s, items=0x%x stride=0x%x num=%d",
          c.chunked ? "chunked" : "flat",
          c.itemSize, static_cast<unsigned>(sizeof(void*)), c.numElements);
    return true;
}

void ObjectArray::Reset() noexcept {
    S() = State{};
}

bool ObjectArray::Probe(uintptr_t addr) noexcept {
    if (addr == 0) return false;
    SafeMemory::ScopedSigSegvGuard guard;
    bool ok = false;
    guard.Try([&] { ok = TryAt(addr).ok; });
    return ok;
}

bool ObjectArray::ProbeNoGuard(uintptr_t addr) noexcept {
    if (addr == 0) return false;
    return TryAt(addr).ok;
}

bool ObjectArray::ProbeReport(uintptr_t addr, std::vector<std::string>& report) noexcept {
    auto add = [&](const char* fmt, ...) {
        char buf[224];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        report.emplace_back(buf);
        // Mirror to logcat so the trace is also `adb logcat -s Dumper7` copyable.
        __android_log_print(ANDROID_LOG_INFO, "Dumper7", "[probe] %s", buf);
    };

    if (addr == 0) { add("addr is null"); return false; }
    add("probing 0x%lx", (unsigned long)addr);

    // SafeMemory state snapshot — cheapest thing to check first.
    add("SafeMemory: initialized=%d libBase=0x%lx span=0x%lx",
        (int)SafeMemory::IsInitialized(),
        (unsigned long)SafeMemory::LibBase(),
        (unsigned long)SafeMemory::LibSize());

    SafeMemory::ScopedSigSegvGuard guard;

    auto attempt = [&](uintptr_t base, const char* tag) -> bool {
        bool layoutOk = false;
        bool chunked  = false;
        int32_t numEl = 0, maxEl = 0, numCh = 0, maxCh = 0;
        uintptr_t chunksPtr = 0;

        guard.Try([&] {
            auto rNum = ReadStruct<int32_t>  (base + Off::FChunkedFixedUObjectArray::NumElements);
            auto rMax = ReadStruct<int32_t>  (base + Off::FChunkedFixedUObjectArray::MaxElements);
            auto rNCh = ReadStruct<int32_t>  (base + Off::FChunkedFixedUObjectArray::NumChunks);
            auto rMCh = ReadStruct<int32_t>  (base + Off::FChunkedFixedUObjectArray::MaxChunks);
            auto rPtr = ReadStruct<uintptr_t>(base + Off::FChunkedFixedUObjectArray::Objects);
            numEl     = rNum.value_or(-1);
            maxEl     = rMax.value_or(-1);
            numCh     = rNCh.value_or(-1);
            maxCh     = rMCh.value_or(-1);
            chunksPtr = rPtr.value_or(0);
            layoutOk  = LooksLikeChunked(base);
            if (layoutOk) chunked = true;
            if (!layoutOk) layoutOk = LooksLikeFlat(base);
        });

        add("[%s 0x%lx] num=%d max=%d nCh=%d maxCh=%d Objects=0x%lx -> %s%s",
            tag, (unsigned long)base, numEl, maxEl, numCh, maxCh,
            (unsigned long)chunksPtr,
            layoutOk ? (chunked ? "chunked" : "flat") : "no-layout",
            layoutOk ? "" : " (layout heuristic rejected)");

        if (!layoutOk) return false;

        bool stride18 = false, stride20 = false;
        guard.Try([&] {
            stride18 = ProbeStride(chunked, chunksPtr,
                                   Off::FChunkedFixedUObjectArray::ElementsPerChunk,
                                   0x18, numEl);
            if (!stride18) {
                stride20 = ProbeStride(chunked, chunksPtr,
                                       Off::FChunkedFixedUObjectArray::ElementsPerChunk,
                                       0x20, numEl);
            }
        });

        if (!stride18 && !stride20) {
            // Re-check first item details to explain why stride probes rejected.
            uintptr_t firstChunk = 0, firstObj = 0, vtable = 0;
            int32_t   internalIdx = -1;
            bool      vtableExec  = false;
            guard.Try([&] {
                auto fc = SafeMemory::SafeReadAny<uintptr_t>(chunksPtr);
                if (fc) firstChunk = *fc;
                if (firstChunk) {
                    auto fo = SafeMemory::SafeReadAny<uintptr_t>(firstChunk);
                    if (fo) firstObj = *fo;
                }
                if (firstObj) {
                    auto vt = SafeMemory::SafeReadAny<uintptr_t>(firstObj);
                    if (vt) {
                        vtable     = *vt;
                        vtableExec = SafeMemory::IsExecutable(vtable, sizeof(uintptr_t));
                    }
                    auto idx = SafeMemory::SafeReadAny<int32_t>(firstObj + Off::UObject::InternalIndex);
                    if (idx) internalIdx = *idx;
                }
            });
            add("  stride probes failed. firstChunk=0x%lx firstObj=0x%lx vtable=0x%lx (exec=%d) internalIdx[0]=%d",
                (unsigned long)firstChunk, (unsigned long)firstObj,
                (unsigned long)vtable, (int)vtableExec, internalIdx);
            return false;
        }

        add("  stride passed: %s", stride18 ? "0x18" : "0x20");
        return true;
    };

    if (attempt(addr, "as-inner")) return true;
    if (attempt(addr + Off::FUObjectArray::ObjObjects, "as-FUObjectArray+0x10")) return true;
    add("rejected: neither addr nor addr+0x10 looks like FUObjectArray inner");
    return false;
}

bool      ObjectArray::IsInitialized() noexcept { return S().initialized; }
bool      ObjectArray::IsChunked()     noexcept { return S().chunked; }
int32_t   ObjectArray::ItemSize()      noexcept { return S().itemSize; }
int32_t   ObjectArray::Num()           noexcept { return S().initialized ? S().numElements : 0; }
uintptr_t ObjectArray::InnerAddress()  noexcept { return S().inner; }
uintptr_t ObjectArray::SourceAddress() noexcept { return S().source; }

void ObjectArray::SetDecryption(DecryptFn fn) noexcept {
    S().decrypt = std::move(fn);
}

UObject* ObjectArray::GetByIndex(int32_t index) noexcept {
    State& s = S();
    if (!s.initialized) return nullptr;
    if (index < 0 || index >= s.numElements) return nullptr;

    uintptr_t itemAddr = s.chunked
            ? ItemAddrChunked(s.objectsPtr, index, s.itemSize, s.elementsPerChunk)
            : ItemAddrFlat   (s.objectsPtr, index, s.itemSize);
    if (itemAddr == 0) return nullptr;

    auto raw = SafeMemory::SafeReadAny<void*>(itemAddr);
    if (!raw || *raw == nullptr) return nullptr;
    return Decrypt(*raw);
}

}  // namespace UE
