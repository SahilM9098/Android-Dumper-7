// =============================================================================
// Engine/PatternScanner.cpp
// -----------------------------------------------------------------------------
// Tokenizer + scanner. Tokens are whitespace-separated. Each token is either
// "??" (full-byte wildcard) or two hex chars where each char may itself be
// "?" (nibble wildcard). The mask byte encodes which nibbles must match.
// =============================================================================

#include "PatternScanner.h"

#include <cstring>

namespace PatternScanner {

namespace {

INTERNAL int HexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

INTERNAL bool IsHexOrWild(char c) noexcept {
    return c == '?' || HexDigit(c) >= 0;
}

}  // namespace

Pattern Parse(const char* sig) noexcept {
    Pattern p;
    if (sig == nullptr) return p;

    while (*sig) {
        while (*sig == ' ' || *sig == '\t' || *sig == ',') ++sig;
        if (*sig == 0) break;
        if (!IsHexOrWild(sig[0]) || !IsHexOrWild(sig[1])) {
            // unknown token; skip a single char and continue
            ++sig;
            continue;
        }

        char hi = sig[0];
        char lo = sig[1];
        sig += 2;

        uint8_t mask = 0;
        uint8_t byte = 0;
        if (hi == '?') {
            mask &= 0x0F;
        } else {
            mask |= 0xF0;
            byte |= static_cast<uint8_t>(HexDigit(hi) << 4);
        }
        if (lo == '?') {
            // mask low nibble already 0
        } else {
            mask |= 0x0F;
            byte |= static_cast<uint8_t>(HexDigit(lo));
        }

        p.bytes.push_back(byte);
        p.mask.push_back(mask);
    }
    return p;
}

uintptr_t Scan(uintptr_t start, size_t size, const Pattern& pat) noexcept {
    if (pat.empty() || size < pat.size()) return 0;
    const uint8_t* mem  = reinterpret_cast<const uint8_t*>(start);
    const size_t   plen = pat.size();
    const size_t   stop = size - plen;

    for (size_t i = 0; i <= stop; ++i) {
        bool match = true;
        for (size_t j = 0; j < plen; ++j) {
            uint8_t m = pat.mask[j];
            if (m == 0) continue;  // full wildcard
            if ((mem[i + j] & m) != (pat.bytes[j] & m)) {
                match = false;
                break;
            }
        }
        if (match) return start + i;
    }
    return 0;
}

std::vector<uintptr_t> ScanAll(uintptr_t start, size_t size,
                               const Pattern& pat, size_t maxHits) noexcept {
    std::vector<uintptr_t> hits;
    if (pat.empty() || size < pat.size()) return hits;
    const uint8_t* mem  = reinterpret_cast<const uint8_t*>(start);
    const size_t   plen = pat.size();
    const size_t   stop = size - plen;

    for (size_t i = 0; i <= stop && hits.size() < maxHits; ++i) {
        bool match = true;
        for (size_t j = 0; j < plen; ++j) {
            uint8_t m = pat.mask[j];
            if (m == 0) continue;
            if ((mem[i + j] & m) != (pat.bytes[j] & m)) {
                match = false;
                break;
            }
        }
        if (match) hits.push_back(start + i);
    }
    return hits;
}

std::string Describe(const Pattern& pat) noexcept {
    std::string out;
    out.reserve(pat.size() * 3);
    char buf[4];
    for (size_t i = 0; i < pat.size(); ++i) {
        if (i) out.push_back(' ');
        uint8_t m = pat.mask[i];
        if (m == 0)        { out += "??"; continue; }
        if (m == 0xFF) {
            std::snprintf(buf, sizeof(buf), "%02X", pat.bytes[i]);
            out += buf;
            continue;
        }
        char hi = (m & 0xF0) ? "0123456789ABCDEF"[(pat.bytes[i] >> 4) & 0xF] : '?';
        char lo = (m & 0x0F) ? "0123456789ABCDEF"[ pat.bytes[i]       & 0xF] : '?';
        out.push_back(hi);
        out.push_back(lo);
    }
    return out;
}

}  // namespace PatternScanner
