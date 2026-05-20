// =============================================================================
// Engine/SafeMemory.h
// -----------------------------------------------------------------------------
// Memory-safety primitives for cross-process game-memory reads.
//
//   * SafeRead<T>(addr)        — null + alignment + libUE4-bounds-checked copy.
//                                Returns std::nullopt on any check failure.
//                                Never faults, never throws.
//
//   * SafeReadAny<T>(addr)     — relaxed variant: null + alignment only. For
//                                reads into game-allocated heap (object chunks,
//                                FNamePool blocks) where libUE4 bounds do not
//                                apply. Pair with ScopedSigSegvGuard.
//
//   * ScopedSigSegvGuard       — RAII installer of a SIGSEGV/SIGBUS handler
//                                that converts faults into a longjmp out of
//                                a Try(callable) invocation. Use to guard
//                                per-object steps in bulk walks.
//
// Init() must be called once after libUE4 is located (UE4Info populated).
// =============================================================================

#pragma once

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <optional>
#include <signal.h>
#include <type_traits>

#include "Utility.h"

#define INTERNAL __attribute__((visibility("hidden")))

namespace SafeMemory {

INTERNAL void Init(const UE4Info& info) noexcept;
INTERNAL bool IsInitialized() noexcept;

INTERNAL bool IsInLibUE4(uintptr_t addr, size_t size = 1) noexcept;

// True if `addr` falls inside any PT_LOAD segment marked executable across
// any loaded shared object. Used to validate UObject vtable pointers when
// the engine is split across multiple modules (UE5 case).
INTERNAL bool IsExecutable(uintptr_t addr, size_t size = 1) noexcept;

INTERNAL uintptr_t LibBase() noexcept;
INTERNAL size_t LibSize() noexcept;

template <typename T>
inline std::optional<T> SafeRead(uintptr_t addr) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SafeRead requires a trivially-copyable type");
    if (addr == 0) return std::nullopt;
    if ((addr & (alignof(T) - 1)) != 0) return std::nullopt;
    if (!IsInLibUE4(addr, sizeof(T))) return std::nullopt;
    T out;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return out;
}

template <typename T>
inline std::optional<T> SafeReadAny(uintptr_t addr) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SafeReadAny requires a trivially-copyable type");
    if (addr == 0) return std::nullopt;
    if ((addr & (alignof(T) - 1)) != 0) return std::nullopt;
    T out;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return out;
}

class ScopedSigSegvGuard {
public:
    ScopedSigSegvGuard() noexcept;
    ~ScopedSigSegvGuard() noexcept;

    ScopedSigSegvGuard(const ScopedSigSegvGuard&) = delete;
    ScopedSigSegvGuard& operator=(const ScopedSigSegvGuard&) = delete;

    // Run f() under SIGSEGV/SIGBUS protection. Returns false if a fault was
    // caught (f did not complete); true if f returned normally.
    template <class F>
    bool Try(F&& f) noexcept {
        if (sigsetjmp(s_jmp, 1) != 0) {
            s_active.store(false, std::memory_order_release);
            ++s_faultCount;
            return false;
        }
        s_active.store(true, std::memory_order_release);
        f();
        s_active.store(false, std::memory_order_release);
        return true;
    }

    static int FaultCount() noexcept { return s_faultCount; }
    static void ResetFaultCount() noexcept { s_faultCount = 0; }

private:
    bool installed_ = false;
    struct sigaction prev_segv_{};
    struct sigaction prev_bus_{};

    static thread_local sigjmp_buf s_jmp;
    static thread_local std::atomic<bool> s_active;
    static thread_local int s_faultCount;

    friend void FaultHandler(int sig, siginfo_t* info, void* ctx) noexcept;
};

}  // namespace SafeMemory
