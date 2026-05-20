// =============================================================================
// Core/NameArray.cpp
// -----------------------------------------------------------------------------
// Implementation. The interesting bit is BlocksOffset auto-detection:
//
//   1. Walk candidate offsets within the first 0x200 bytes of the supplied
//      pool address (FRWLock + bookkeeping is rarely larger than that).
//   2. At each 8-byte-aligned candidate, treat the slot as Blocks[]. Read
//      Blocks[0] — must be a non-null pointer into game-heap memory.
//   3. Treat that block as the start of FName entries. Decode the first
//      entry's 2-byte FNameEntryHeader. Length must equal 4 and bIsWide
//      must be 0 (FName index 0 is always narrow "None"). Compare bytes
//      [hdr+0..hdr+3] == "None".
//   4. The first offset that satisfies all of the above is BlocksOffset.
//
// This handles the offset drift across UE versions and platform-dependent
// FRWLock sizes (Linux/Android pthread_rwlock_t vs Win32 SRWLock).
// =============================================================================

#include "NameArray.h"

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

struct State {
    bool      initialized    = false;
    uintptr_t source         = 0;     // address as supplied to Init
    uintptr_t blocksArray    = 0;     // resolved Blocks[] base
    int32_t   blocksOffset   = 0;     // blocksArray - source, when direct
    bool      doubleIndirect = false; // true when source is a pointer-to-Blocks
                                      // (e.g. UE5 GNameBlocksDebug)
    bool      casePreserving = false; // FNameEntryHeader layout — see decoders
    int32_t   lengthShift    = 6;     // bits to shift header right to get length
                                      // standard=6, case-preserving=1, auto-detected
    int32_t   stringOffset   = 2;     // byte offset from entry start to string data
};

INTERNAL State& S() noexcept {
    static State s;
    return s;
}

// "None" is the canonical FName at index 0. Length 4, narrow.
constexpr const char  kSentinelName[]    = "None";
constexpr int32_t     kSentinelLength    = 4;

// Candidate BlocksOffset values — searched in order. Covers the common
// platform/version drift without exhaustively scanning every byte.
constexpr int32_t kCandidateBlockOffsets[] = {
    0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58,
    0x60, 0x68, 0x70, 0x78, 0x80, 0xA0, 0xC0, 0xC8, 0xD0,
};

// =============================================================================
// FNameEntryHeader has TWO possible bit layouts depending on UE build flags:
//
//   Standard (default):
//       bIsWide          : 1   (bit 0)
//       LowercaseProbeHash : 5  (bits 1..5)   — for hash lookups, NOT length
//       Len              : 10  (bits 6..15)  — max 1023
//
//   WITH_CASE_PRESERVING_NAME:
//       bIsWide : 1   (bit 0)
//       Len     : 15  (bits 1..15)            — max 32767
//
// We auto-detect at Init time by trying both decodings and accepting the one
// whose first entry decodes to "None" of length 4. The choice is sticky for
// the rest of the session.
// =============================================================================

struct DecodedHeader {
    bool    wide;
    int32_t length;
};

INTERNAL DecodedHeader DecodeHeaderStandard(uint16_t raw) noexcept {
    DecodedHeader h;
    h.wide   = (raw & 0x1) != 0;
    h.length = static_cast<int32_t>((raw >> 6) & 0x3FF);   // 10 bits
    return h;
}

INTERNAL DecodedHeader DecodeHeaderCasePreserving(uint16_t raw) noexcept {
    DecodedHeader h;
    h.wide   = (raw & 0x1) != 0;
    h.length = static_cast<int32_t>((raw >> 1) & 0x7FFF);  // 15 bits
    return h;
}

INTERNAL int32_t WideDecodeScore(const std::string& s) noexcept {
    if (s.empty()) return -100000;
    int32_t score = 0;
    for (unsigned char c : s) {
        if (c == 0) return -100000;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '/' ||
            c == '.' || c == ':' || c == '-') {
            score += 2;
        } else if (c >= 0x20 && c < 0x7F) {
            score += 1;
        } else if (c == '?') {
            score += 0;
        } else {
            score -= 2;
        }
    }
    return score;
}

INTERNAL bool ReadWideEntryWithStride(uintptr_t entryAddr, int32_t length,
                                      int32_t stride, std::string& outName) noexcept {
    outName.assign(static_cast<size_t>(length), '\0');
    for (int32_t i = 0; i < length; ++i) {
        uint32_t code = 0;
        if (stride == 2) {
            auto u = SafeMemory::SafeReadAny<uint16_t>(
                    entryAddr + 2 + static_cast<uintptr_t>(i) * 2);
            if (!u || *u == 0) { outName.clear(); return false; }
            code = *u;
        } else {
            auto u = SafeMemory::SafeReadAny<uint32_t>(
                    entryAddr + 2 + static_cast<uintptr_t>(i) * 4);
            if (!u || *u == 0) { outName.clear(); return false; }
            code = *u;
        }
        outName[static_cast<size_t>(i)] =
                (code < 0x80) ? static_cast<char>(code) : '?';
    }
    return true;
}

INTERNAL bool ReadWideEntryAuto(uintptr_t entryAddr, int32_t length,
                                std::string& outName) noexcept {
    std::string wide2;
    const bool ok2 = ReadWideEntryWithStride(entryAddr, length, 2, wide2);
    if (ok2 && WideDecodeScore(wide2) >= length) {
        outName = wide2;
        return true;
    }

    std::string wide4;
    const bool ok4 = ReadWideEntryWithStride(entryAddr, length, 4, wide4);
    if (ok4 && (!ok2 || WideDecodeScore(wide4) > WideDecodeScore(wide2))) {
        outName = wide4;
        return true;
    }
    if (ok2) {
        outName = wide2;
        return true;
    }
    outName.clear();
    return false;
}

// Read an entry's header + character data at `entryAddr` using the given
// layout. Returns false on any failure; outName is set to the decoded string.
INTERNAL bool ReadEntryWithLayout(uintptr_t entryAddr, bool casePreserving,
                                  std::string& outName) noexcept {
    auto rawHeader = SafeMemory::SafeReadAny<uint16_t>(entryAddr);
    if (!rawHeader) return false;

    const DecodedHeader hdr = casePreserving
            ? DecodeHeaderCasePreserving(*rawHeader)
            : DecodeHeaderStandard(*rawHeader);

    if (hdr.length < 0 || hdr.length > 1023) return false;
    if (hdr.length == 0) { outName.clear(); return true; }

    outName.assign(static_cast<size_t>(hdr.length), '\0');
    if (!hdr.wide) {
        for (int32_t i = 0; i < hdr.length; ++i) {
            auto c = SafeMemory::SafeReadAny<uint8_t>(entryAddr + 2 + i);
            if (!c) { outName.clear(); return false; }
            outName[i] = static_cast<char>(*c);
        }
        return true;
    }
    // Wide storage is platform/build dependent (commonly 16-bit or 32-bit).
    // Try both strides and keep the plausible ASCII best-effort result.
    return ReadWideEntryAuto(entryAddr, hdr.length, outName);
}

// Compatibility wrapper used by GetName once the layout has been latched
// into the State.
INTERNAL bool ReadEntryAtAddress(uintptr_t entryAddr, std::string& outName) noexcept {
    return ReadEntryWithLayout(entryAddr, S().casePreserving, outName);
}

// Validate that `blocksAt` resolves to a Blocks[] array whose first block's
// first entry decodes to "None". `casePreservingOut` reports which header
// layout produced the match.
INTERNAL bool ValidateAtBlocksAddr(uintptr_t blocksAt,
                                   bool& casePreservingOut) noexcept {
    auto firstBlock = SafeMemory::SafeReadAny<uintptr_t>(blocksAt);
    if (!firstBlock || *firstBlock == 0) return false;
    if (*firstBlock < 0x10000) return false;

    std::string name;
    // Try standard layout first (10-bit length + 5-bit hash).
    if (ReadEntryWithLayout(*firstBlock, /*casePreserving*/ false, name) &&
        name.size() == static_cast<size_t>(kSentinelLength) &&
        std::strncmp(name.c_str(), kSentinelName, kSentinelLength) == 0) {
        casePreservingOut = false;
        return true;
    }
    // Fall back to case-preserving layout (15-bit length).
    if (ReadEntryWithLayout(*firstBlock, /*casePreserving*/ true, name) &&
        name.size() == static_cast<size_t>(kSentinelLength) &&
        std::strncmp(name.c_str(), kSentinelName, kSentinelLength) == 0) {
        casePreservingOut = true;
        return true;
    }
    return false;
}

struct DetectResult {
    bool      ok             = false;
    uintptr_t blocksArray    = 0;     // resolved &Blocks[0]
    int32_t   blocksOffset   = 0;
    bool      doubleIndirect = false;
    bool      casePreserving = false;
};

// Try to resolve an actual Blocks[] base from `poolAddr` under three
// interpretations:
//
//   Mode A  (FNamePool global):       Blocks at poolAddr + offset
//   Mode B  (Blocks[] symbol):        Blocks at poolAddr  (offset 0)
//                                     — a special case of Mode A
//   Mode C  (pointer-to-Blocks):      Blocks at *poolAddr
//                                     — used by UE5's GNameBlocksDebug, which
//                                       is a global pointer that holds the
//                                       address of the real Blocks array
//
// Validation in every mode is the same: read the candidate `&Blocks[0]`,
// dereference Blocks[0] to get block-0's first entry, decode the header,
// and confirm the entry is "None".
INTERNAL DetectResult DetectBlocks(uintptr_t poolAddr) noexcept {
    DetectResult r;
    bool cp = false;

    // Mode C FIRST — read pointer at poolAddr and treat as &Blocks[0]. This
    // is one read + one strict validate (decode "None"). It must run first
    // because Modes A/B can fault while chasing pointer-shaped garbage, and
    // the caller's outer guard would longjmp out of the entire DetectBlocks
    // call — so any later mode would never get a chance to run. Mode C's
    // false-positive risk is negligible: a real FNamePool's first 8 bytes
    // are FRWLock state, not a pointer to a "None" entry.
    auto p = SafeMemory::SafeReadAny<uintptr_t>(poolAddr);
    if (p && *p != 0 && *p >= 0x10000 && ValidateAtBlocksAddr(*p, cp)) {
        r.ok = true; r.blocksArray = *p;
        r.blocksOffset = 0; r.doubleIndirect = true; r.casePreserving = cp;
        return r;
    }

    // Mode A — curated candidate list. Fast, narrow: each value is a known
    // FNamePool layout offset across UE versions. May fault on garbage.
    for (int32_t off : kCandidateBlockOffsets) {
        if (ValidateAtBlocksAddr(poolAddr + off, cp)) {
            r.ok = true; r.blocksArray = poolAddr + off;
            r.blocksOffset = off; r.doubleIndirect = false; r.casePreserving = cp;
            return r;
        }
    }

    // Mode B — brute scan. Last resort. Each iteration may chase a fake
    // heap-pointer-shaped value into unmapped memory and fault, in which
    // case the caller's guard aborts us — but that's fine, C and A already
    // covered the common cases.
    for (int32_t off = 0; off < 0x200; off += 8) {
        if (ValidateAtBlocksAddr(poolAddr + off, cp)) {
            r.ok = true; r.blocksArray = poolAddr + off;
            r.blocksOffset = off; r.doubleIndirect = false; r.casePreserving = cp;
            return r;
        }
    }

    return r;
}

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool NameArray::Init(uintptr_t addr) noexcept {
    State& s = S();
    s.initialized = false;
    s.source      = addr;

    if (addr == 0) {
        DLOGE("NameArray::Init: null address");
        return false;
    }

    SafeMemory::ScopedSigSegvGuard guard;
    DetectResult d{};
    guard.Try([&] { d = DetectBlocks(addr); });

    if (!d.ok) {
        DLOGE("NameArray::Init: detection failed at 0x%lx", (unsigned long)addr);
        return false;
    }

    s.blocksOffset    = d.blocksOffset;
    s.blocksArray     = d.blocksArray;
    s.doubleIndirect  = d.doubleIndirect;
    s.casePreserving  = d.casePreserving;
    s.lengthShift     = d.casePreserving ? 1 : 6;  // default; auto-detect may override
    s.stringOffset    = 2;                          // default; auto-detect may override
    s.initialized     = true;
    Off::FNamePool::BlocksOffset = d.blocksOffset;
    Off::Resolved::GNames = addr;
    Off::InSDK::bIsCasePreserving = d.casePreserving;
    Off::InSDK::bUsesNamePool = true;

    // --- Auto-detect FNameEntry header shift count ---
    // Dumper-7 approach: find the "ByteProperty" entry (second entry in block 0
    // after "None"), read its 16-bit header, and shift right until the value
    // equals 12 (strlen("ByteProperty")). The shift count tells us how many
    // low bits are flags/hash before the length field starts.
    {
        SafeMemory::ScopedSigSegvGuard detectGuard;
        detectGuard.Try([&] {
            auto blockPtr = SafeMemory::SafeReadAny<uintptr_t>(s.blocksArray);
            if (!blockPtr || *blockPtr == 0) return;

            const uintptr_t block0 = *blockPtr;

            // "None" is 4 chars. Entry = header(2) + "None"(4) = 6 bytes.
            // Aligned to stride(2) = 6 bytes → padded to 6 (already aligned).
            // But some builds have extra padding. Scan for "ByteProperty" start.
            constexpr uint32_t kBytePropertyMagic = 0x65747942; // "Byte" as uint32 LE
            constexpr int32_t kBytePropertyLen = 12;

            uintptr_t bpEntry = 0;
            // Scan from offset 4 to 32 looking for "Byte" at the string position
            for (int32_t probe = 4; probe <= 32; ++probe) {
                // Try string at probe + 0 (header at probe - headerSize)
                // and string at probe + 2 (header at probe)
                for (int32_t strOff = 0; strOff <= 6; strOff += 2) {
                    auto val = SafeMemory::SafeReadAny<uint32_t>(block0 + probe + strOff);
                    if (val && *val == kBytePropertyMagic) {
                        // Found "Byte" at block0 + probe + strOff
                        // The entry starts at block0 + probe + strOff - headerSize
                        // where headerSize is the string offset (2 or 4 or 6)
                        // Try each possible header position before the string
                        for (int32_t hdrOff = 2; hdrOff <= 6; hdrOff += 2) {
                            uintptr_t candidateEntry = block0 + probe + strOff - hdrOff;
                            if (candidateEntry <= block0) continue;
                            auto hdr = SafeMemory::SafeReadAny<uint16_t>(candidateEntry);
                            if (!hdr || *hdr == 0) continue;
                            // Try shifting until we get 12
                            uint16_t h = *hdr;
                            for (int32_t shift = 1; shift <= 15; ++shift) {
                                if ((h >> shift) == kBytePropertyLen) {
                                    bpEntry = candidateEntry;
                                    // Determine the string offset within entry
                                    int32_t entryStrOff = hdrOff;
                                    // Update the header decoder based on shift count
                                    // shift=1 → case-preserving (len at bits 1..15)
                                    // shift=6 → standard (len at bits 6..15)
                                    // anything else → custom
                                    Off::FNameEntry::HeaderOffset = 0;
                                    Off::FNameEntry::NameOffset = entryStrOff;
                                    Off::FNamePool::Stride = 2;
                                    // Override the case-preserving flag based on shift
                                    s.casePreserving = (shift == 1);
                                    Off::InSDK::bIsCasePreserving = s.casePreserving;
                                    DLOGI("NameArray::Init: auto-detected header shift=%d strOff=%d (ByteProperty entry @ block0+0x%lx)",
                                          shift, entryStrOff,
                                          (unsigned long)(candidateEntry - block0));
                                    goto detect_done;
                                }
                            }
                        }
                    }
                }
            }
            detect_done:;
        });

        // Now verify: try to read name at index 0 — should be "None"
        std::string testNone = GetName(0);
        if (testNone == "None") {
            DLOGI("NameArray::Init: verification passed — GetName(0)=\"None\"");
        } else {
            DLOGI("NameArray::Init: verification note — GetName(0)=\"%s\" (may need stride tuning)",
                  testNone.c_str());
            // Try alternative strides
            for (int32_t tryStride : {2, 4}) {
                Off::FNamePool::Stride = tryStride;
                testNone = GetName(0);
                if (testNone == "None") {
                    DLOGI("NameArray::Init: fixed with stride=%d", tryStride);
                    break;
                }
            }
        }
    }

    DLOGI("NameArray::Init: source=0x%lx -> blocksArray=0x%lx (offset=0x%x %s, %s)",
          (unsigned long)addr, (unsigned long)d.blocksArray, d.blocksOffset,
          d.doubleIndirect ? "via *source / GNameBlocksDebug" : "direct",
          d.casePreserving ? "case-preserving header" : "standard header");
    return true;
}

void NameArray::Reset() noexcept {
    S() = State{};
}

bool      NameArray::IsInitialized()      noexcept { return S().initialized; }
int32_t   NameArray::BlocksOffset()       noexcept { return S().blocksOffset; }
uintptr_t NameArray::SourceAddress()      noexcept { return S().source; }
uintptr_t NameArray::BlocksArrayAddress() noexcept { return S().blocksArray; }
bool      NameArray::IsDoubleIndirect()   noexcept { return S().doubleIndirect; }
bool      NameArray::IsCasePreserving()   noexcept { return S().casePreserving; }

bool NameArray::Probe(uintptr_t addr) noexcept {
    if (addr == 0) return false;
    SafeMemory::ScopedSigSegvGuard guard;
    bool ok = false;
    guard.Try([&] { ok = DetectBlocks(addr).ok; });
    return ok;
}

bool NameArray::ProbeNoGuard(uintptr_t addr) noexcept {
    if (addr == 0) return false;
    return DetectBlocks(addr).ok;
}

bool NameArray::ProbeReport(uintptr_t addr, std::vector<std::string>& report) noexcept {
    auto add = [&](const char* fmt, ...) {
        char buf[224];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        report.emplace_back(buf);
        DLOGI("[name-probe] %s", buf);
    };

    if (addr == 0) { add("addr is null"); return false; }
    add("probing 0x%lx", static_cast<unsigned long>(addr));
    add("SafeMemory: initialized=%d libBase=0x%lx",
        (int)SafeMemory::IsInitialized(),
        (unsigned long)SafeMemory::LibBase());

    SafeMemory::ScopedSigSegvGuard guard;
    int32_t hitOff = -1;
    int32_t bestOff = -1;
    uintptr_t bestFirstBlock = 0;
    std::string bestName;
    bool bestCasePreserving = false;

    guard.Try([&] {
        for (int32_t off : kCandidateBlockOffsets) {
            uintptr_t blocksAt = addr + off;
            auto firstBlock = SafeMemory::SafeReadAny<uintptr_t>(blocksAt);
            if (!firstBlock || *firstBlock == 0 || *firstBlock < 0x10000) continue;

            std::string nm;
            // Standard layout first.
            if (ReadEntryWithLayout(*firstBlock, false, nm) && !nm.empty()) {
                if (bestOff < 0 || nm == kSentinelName) {
                    bestOff = off; bestFirstBlock = *firstBlock; bestName = nm;
                    bestCasePreserving = false;
                }
                if (nm == kSentinelName) { hitOff = off; break; }
            }
            // Case-preserving layout.
            if (ReadEntryWithLayout(*firstBlock, true, nm) && !nm.empty()) {
                if (bestOff < 0 || nm == kSentinelName) {
                    bestOff = off; bestFirstBlock = *firstBlock; bestName = nm;
                    bestCasePreserving = true;
                }
                if (nm == kSentinelName) { hitOff = off; break; }
            }
        }
    });

    if (bestOff >= 0) {
        add("best direct candidate: BlocksOffset=0x%x firstBlock=0x%lx firstName=\"%s\" (%s)",
            bestOff, (unsigned long)bestFirstBlock, bestName.c_str(),
            bestCasePreserving ? "case-preserving header" : "standard header");
    } else {
        add("no direct offset produced a readable first entry");
    }

    if (hitOff >= 0) {
        add("validated (direct): BlocksOffset=0x%x first entry == \"None\" (%s)",
            hitOff, bestCasePreserving ? "case-preserving header" : "standard header");
        return true;
    }

    // Mode C — try double-indirection (GNameBlocksDebug case).
    uintptr_t indirectBlocks = 0;
    bool indirectMatch = false;
    bool indirectCasePreserving = false;
    std::string indirectName;
    guard.Try([&] {
        auto p = SafeMemory::SafeReadAny<uintptr_t>(addr);
        if (!p || *p == 0) return;
        indirectBlocks = *p;
        bool cp = false;
        if (ValidateAtBlocksAddr(indirectBlocks, cp)) {
            indirectMatch = true;
            indirectCasePreserving = cp;
        }
        // Read first name with whichever layout matched (or standard for diag).
        auto firstBlock = SafeMemory::SafeReadAny<uintptr_t>(indirectBlocks);
        if (firstBlock && *firstBlock) {
            std::string nm;
            ReadEntryWithLayout(*firstBlock, indirectCasePreserving, nm);
            indirectName = nm;
        }
    });

    if (indirectBlocks) {
        add("double-indirect: *source=0x%lx firstName=\"%s\"",
            (unsigned long)indirectBlocks, indirectName.c_str());
    }
    if (indirectMatch) {
        add("validated (indirect): treating *source as &Blocks[0] (%s)",
            indirectCasePreserving ? "case-preserving header" : "standard header");
        return true;
    }

    add("rejected: neither direct nor indirect interpretation produced \"None\"");
    return false;
}

std::vector<std::pair<int32_t, std::string>>
NameArray::WalkBlock(int32_t blockIndex, int32_t maxCount) noexcept {
    std::vector<std::pair<int32_t, std::string>> out;
    State& s = S();
    if (!s.initialized || blockIndex < 0 || maxCount <= 0) return out;

    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        auto blockPtr = SafeMemory::SafeReadAny<uintptr_t>(
                s.blocksArray + blockIndex * sizeof(uintptr_t));
        if (!blockPtr || *blockPtr == 0) return;

        const uintptr_t base = *blockPtr;
        uintptr_t cur = base;

        // Each block is fixed-size in UE4/5 (typically 64KiB / Stride=2 ⇒
        // 32K possible entry slots). Walk until we exhaust maxCount, hit a
        // zero header, or fail to decode.
        const uintptr_t maxBytes = static_cast<uintptr_t>(1)
                                   << Off::FNamePool::BlockOffsetBits;
        while (static_cast<int32_t>(out.size()) < maxCount &&
               (cur - base) < maxBytes) {
            auto rawHeader = SafeMemory::SafeReadAny<uint16_t>(cur);
            if (!rawHeader) break;
            if (*rawHeader == 0) break;  // zero-padding tail of block

            const DecodedHeader hdr = s.casePreserving
                    ? DecodeHeaderCasePreserving(*rawHeader)
                    : DecodeHeaderStandard(*rawHeader);
            if (hdr.length <= 0 || hdr.length > 1023) break;

            std::string name;
            if (!ReadEntryWithLayout(cur, s.casePreserving, name)) break;

            const int32_t idx = static_cast<int32_t>(
                    (blockIndex << Off::FNamePool::BlockOffsetBits) |
                    ((cur - base) / Off::FNamePool::Stride));
            out.emplace_back(idx, std::move(name));

            uintptr_t entryBytes = 2 + (hdr.wide
                    ? static_cast<uintptr_t>(hdr.length) * 2
                    : static_cast<uintptr_t>(hdr.length));
            // Align up to Stride (2 bytes).
            entryBytes = (entryBytes + (Off::FNamePool::Stride - 1))
                         & ~(uintptr_t(Off::FNamePool::Stride - 1));
            cur += entryBytes;
        }
    });
    return out;
}

std::string NameArray::GetName(int32_t comparisonIndex) noexcept {
    State& s = S();
    if (!s.initialized || comparisonIndex < 0) return {};

    // --- Name cache ---
    // GetName is called millions of times during a dump. Most games have
    // <200K unique FName indices, but the same indices (e.g. "IntProperty",
    // "Class", "None") are resolved thousands of times. Caching here
    // eliminates redundant FNamePool block walks and SafeMemory guard setup.
    static std::unordered_map<int32_t, std::string> nameCache;
    static std::mutex nameCacheMu;
    {
        std::lock_guard<std::mutex> lk(nameCacheMu);
        auto it = nameCache.find(comparisonIndex);
        if (it != nameCache.end()) return it->second;
    }

    const uint32_t blockIndex   = static_cast<uint32_t>(comparisonIndex)
                                  >> Off::FNamePool::BlockOffsetBits;
    const uint32_t entryOffset  = static_cast<uint32_t>(comparisonIndex)
                                  &  Off::FNamePool::BlockOffsetMask;

    SafeMemory::ScopedSigSegvGuard guard;
    std::string out;
    guard.Try([&] {
        auto blockPtr = SafeMemory::SafeReadAny<uintptr_t>(
                s.blocksArray + blockIndex * sizeof(uintptr_t));
        if (!blockPtr || *blockPtr == 0) return;

        uintptr_t entryAddr = *blockPtr + entryOffset * Off::FNamePool::Stride;
        ReadEntryAtAddress(entryAddr, out);
    });

    // Cache the result (even empty strings — avoids re-trying bad indices)
    if (!out.empty()) {
        std::lock_guard<std::mutex> lk(nameCacheMu);
        nameCache.emplace(comparisonIndex, out);
    }
    return out;
}

}  // namespace UE
