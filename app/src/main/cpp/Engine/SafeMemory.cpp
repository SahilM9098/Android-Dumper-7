// =============================================================================
// Engine/SafeMemory.cpp
// -----------------------------------------------------------------------------
// Implementation of SafeMemory primitives. Tracks the loaded segments of
// libUE4.so (collected via dl_iterate_phdr) for the bounds check used by
// SafeRead, and installs/restores a SIGSEGV+SIGBUS handler for the
// ScopedSigSegvGuard RAII type. The handler longjmps out of a Try() call
// when a guarded read faults; outside of any Try() it restores the default
// handler and re-raises.
// =============================================================================

#include "SafeMemory.h"

#include <android/log.h>
#include <cstring>
#include <link.h>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace SafeMemory {

namespace {

struct LibSegment {
    uintptr_t start;
    uintptr_t end;
};

struct LibState {
    bool      initialized = false;
    uintptr_t base        = 0;
    size_t    span        = 0;   // base..(base+span) bounding box
    std::vector<LibSegment> segments;       // libUE4 readable segments
    std::vector<LibSegment> execSegments;   // executable segments across ALL libraries
};

INTERNAL LibState& State() noexcept {
    static LibState s;
    return s;
}

INTERNAL void CollectSegments(uintptr_t base, LibState& s) noexcept {
    s.segments.clear();
    s.execSegments.clear();

    struct Ctx { uintptr_t base; LibState* out; };
    Ctx ctx{ base, &s };

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        const bool isTargetLib = (info->dlpi_addr == c->base);

        // First pass: PT_LOAD segments — text (R+X) and pure rodata (R).
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            uintptr_t segStart = info->dlpi_addr + ph.p_vaddr;
            uintptr_t segEnd   = segStart + ph.p_memsz;

            if (isTargetLib && (ph.p_flags & PF_R) != 0) {
                c->out->segments.push_back({ segStart, segEnd });
            }
            const bool readable = (ph.p_flags & PF_R) != 0;
            const bool writable = (ph.p_flags & PF_W) != 0;
            if (readable && !writable) {
                c->out->execSegments.push_back({ segStart, segEnd });
            }
        }

        // Second pass: PT_GNU_RELRO — declares the sub-range of a PT_LOAD
        // that is read-only AFTER dynamic relocations are applied. Modern
        // toolchains park C++ vtables in .data.rel.ro inside this range,
        // which means the parent PT_LOAD has PF_W (the loader needs write
        // access during init), but at runtime the pages have been mprotect'd
        // read-only. PF flags on the program header don't reflect that, so
        // we add the RELRO range explicitly as a valid vtable host.
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_GNU_RELRO) continue;
            uintptr_t segStart = info->dlpi_addr + ph.p_vaddr;
            uintptr_t segEnd   = segStart + ph.p_memsz;
            c->out->execSegments.push_back({ segStart, segEnd });
        }

        return 0;  // continue iterating all libraries
    }, &ctx);

    uintptr_t lo = ~uintptr_t(0), hi = 0;
    for (const auto& seg : s.segments) {
        if (seg.start < lo) lo = seg.start;
        if (seg.end   > hi) hi = seg.end;
    }
    s.span = s.segments.empty() ? 0 : (hi - lo);
}

}  // namespace

void Init(const UE4Info& info) noexcept {
    LibState& s = State();
    if (info.base == 0) {
        DLOGE("SafeMemory::Init called with empty UE4Info");
        return;
    }
    // Always re-collect, not just on first call. The host process can load
    // additional shared objects after our initial Init; without a refresh,
    // execSegments would miss them and IsExecutable rejects perfectly valid
    // vtable pointers in late-loaded libraries.
    s.base = info.base;
    s.initialized = true;
    CollectSegments(info.base, s);
    DLOGI("SafeMemory::Init base=%p segments=%zu execAcrossLibs=%zu span=0x%zx",
          (void*)s.base, s.segments.size(), s.execSegments.size(), s.span);
}

bool IsInitialized() noexcept {
    return State().initialized;
}

bool IsInLibUE4(uintptr_t addr, size_t size) noexcept {
    const LibState& s = State();
    if (!s.initialized) return false;
    if (size == 0) size = 1;
    uintptr_t end = addr + size;
    if (end < addr) return false;  // overflow
    for (const auto& seg : s.segments) {
        if (addr >= seg.start && end <= seg.end) return true;
    }
    return false;
}

uintptr_t LibBase() noexcept { return State().base; }
size_t    LibSize() noexcept { return State().span; }

bool IsExecutable(uintptr_t addr, size_t size) noexcept {
    const LibState& s = State();
    if (!s.initialized) return false;
    if (size == 0) size = 1;
    uintptr_t end = addr + size;
    if (end < addr) return false;
    for (const auto& seg : s.execSegments) {
        if (addr >= seg.start && end <= seg.end) return true;
    }

    // Cache miss — the host may have loaded a library after our last
    // CollectSegments. Run a one-shot dl_iterate_phdr to confirm. If we
    // hit, return true (without mutating the cache; caller's next Init
    // refresh picks it up).
    struct Ctx { uintptr_t addr; uintptr_t end; bool found; };
    Ctx ctx{ addr, end, false };
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            const bool isLoad  = (ph.p_type == PT_LOAD);
            const bool isRelro = (ph.p_type == PT_GNU_RELRO);
            if (!isLoad && !isRelro) continue;

            // For PT_LOAD: accept text (R+X) and rodata (R-only). Skip RW.
            // For PT_GNU_RELRO: accept the whole declared range (vtables in
            // .data.rel.ro live here, mprotect'd read-only at runtime).
            if (isLoad) {
                const bool readable = (ph.p_flags & PF_R) != 0;
                const bool writable = (ph.p_flags & PF_W) != 0;
                if (!readable || writable) continue;
            }
            uintptr_t segStart = info->dlpi_addr + ph.p_vaddr;
            uintptr_t segEnd   = segStart + ph.p_memsz;
            if (c->addr >= segStart && c->end <= segEnd) {
                c->found = true;
                return 1;
            }
        }
        return 0;
    }, &ctx);
    return ctx.found;
}

// ---- ScopedSigSegvGuard --------------------------------------------------

thread_local sigjmp_buf       ScopedSigSegvGuard::s_jmp{};
thread_local std::atomic<bool> ScopedSigSegvGuard::s_active{false};
thread_local int              ScopedSigSegvGuard::s_faultCount{0};

void FaultHandler(int sig, siginfo_t* /*info*/, void* /*ctx*/) noexcept {
    if (ScopedSigSegvGuard::s_active.load(std::memory_order_acquire)) {
        ScopedSigSegvGuard::s_active.store(false, std::memory_order_release);
        siglongjmp(ScopedSigSegvGuard::s_jmp, sig);
    }
    // Outside of any guard scope — restore default and re-raise.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}

ScopedSigSegvGuard::ScopedSigSegvGuard() noexcept {
    struct sigaction sa{};
    sa.sa_sigaction = &FaultHandler;
    sa.sa_flags     = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, &prev_segv_) == 0 &&
        sigaction(SIGBUS,  &sa, &prev_bus_)  == 0) {
        installed_ = true;
    } else {
        DLOGE("ScopedSigSegvGuard: sigaction install failed");
    }
}

ScopedSigSegvGuard::~ScopedSigSegvGuard() noexcept {
    s_active.store(false, std::memory_order_release);
    if (installed_) {
        sigaction(SIGSEGV, &prev_segv_, nullptr);
        sigaction(SIGBUS,  &prev_bus_,  nullptr);
    }
}

}  // namespace SafeMemory
