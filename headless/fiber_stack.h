// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/mman.h>

namespace Eden {
// Keep the full guest-callback stack size. Anonymous CPU mappings use the console's
// flexible pool, leaving direct memory for guest RAM and GPU resources. Guard pages
// surround the usable stack. Exhaustion falls back to the existing heap domain.
//
// The pool is a few hundred MiB, and whatever else maps anonymous memory shares it. Each stack also
// asks for PoolFloor on the side and gives it straight back, so the stacks stop while that much is
// left. Both are read-write from the start: that is the request the pool is known to refuse when it
// has no room (the GPU's 1 GiB table, CMakeLists.txt), and a refusal sends a stack to the heap.
// Only whole mappings go back: part of one has never been given back on the console, and fibers are
// made in every session.
//
// Each stack was 2 MiB, and the pool's 448 MiB less the app's own 140 MiB and PoolFloor is 122 such
// stacks: past them every run put its stacks in the heap, in the direct pool (EDEN_FIBER_STACK at 128
// allocations: 244 MiB flexible, 12 MiB fallback, runs 40-53). At 512 KiB, upstream's size for its
// other PlayStation target, they all fit and flexible memory can shrink (sce_sys/param.json). A
// stack in the pool overflows into its guard page, a crash at a known address, not into a neighbour.
class FiberStack {
public:
    FiberStack() = default;
    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;
    ~FiberStack() {
        if (mapping) {
            if (munmap(mapping, bytes + 2 * GuardBytes)) std::abort();
            flexible_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        } else if (fallback) {
            if (std::memcmp(fallback.get(), Canary.data(), CanaryBytes) ||
                std::memcmp(base + bytes, Canary.data(), CanaryBytes)) std::abort();
            fallback_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
    }
    void Allocate(std::size_t size) {
        if (!size || size % GuardBytes || bytes) std::abort();
        bytes = size;
        const std::size_t span = bytes + 2 * GuardBytes;
        void* reserved = Map(span);
        if (reserved != MAP_FAILED) {
            auto* first = static_cast<unsigned char*>(reserved);
            auto* usable = first + GuardBytes;
            void* spare = Map(PoolFloor);
            const bool room = spare != MAP_FAILED;
            if (room && munmap(spare, PoolFloor)) std::abort();
            if (room && CpuRange(reserved, span) && mprotect(first, GuardBytes, PROT_NONE) == 0 &&
                mprotect(usable + bytes, GuardBytes, PROT_NONE) == 0) {
                mapping = reserved;
                base = usable;
                flexible_bytes.fetch_add(bytes, std::memory_order_relaxed);
            } else if (munmap(reserved, span)) {
                std::abort();
            }
        }
        if (!base) {
            fallback.reset(new unsigned char[bytes + 2 * CanaryBytes]);
            base = fallback.get() + CanaryBytes;
            std::memcpy(fallback.get(), Canary.data(), CanaryBytes);
            std::memcpy(base + bytes, Canary.data(), CanaryBytes);
            fallback_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        const auto count = allocations.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((count & (count - 1)) == 0) {
            std::fprintf(stderr, "EDEN_FIBER_STACK allocations=%llu flexible=%llu fallback=%llu stack_bytes=%zu\n",
                         count, flexible_bytes.load(), fallback_bytes.load(), bytes);
        }
    }
    // EDEN_FIBER_STACK_KIB (env.txt), whole guard pages, from 256 KiB to 2 MiB. Read at the first fiber,
    // after env.txt: every stack of a run has one size, which the fiber code takes from here too.
    static std::size_t StackBytes() noexcept {
        static const std::size_t chosen = [] {
            const char* const asked = std::getenv("EDEN_FIBER_STACK_KIB");
            const std::size_t kib = asked ? std::strtoull(asked, nullptr, 10) : 512;
            const std::size_t size = (kib << 10) / GuardBytes * GuardBytes;
            return size < (std::size_t{256} << 10) || size > (std::size_t{2} << 20) ? std::size_t{512} << 10 : size;
        }();
        return chosen;
    }
    unsigned char* data() noexcept { return base; }
    std::size_t size() const noexcept { return bytes; }
    inline static std::atomic<unsigned long long> flexible_bytes{}, fallback_bytes{}, allocations{};
    static constexpr std::size_t GuardBytes = 16384;
private:
    inline static constexpr std::array<unsigned long long, 2> Canary{
        0x8d569a12e3c047bfULL, 0x729db60f24e1ac83ULL};
    static constexpr std::size_t CanaryBytes = sizeof(Canary);
    static constexpr std::size_t PoolFloor = std::size_t{64} << 20;
    // As the app's other CPU memory (src/memory_pages.cpp): a mapping that names no address goes
    // to RADV's 32-bit window at 0x2_0000_0000, and the driver's device memory starts at 0x40_0000_0000.
    static constexpr std::uintptr_t MappingHint = 0x1000000000ull;
    static void* Map(std::size_t size) noexcept {
        return mmap(reinterpret_cast<void*>(MappingHint), size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    static bool CpuRange(const void* at, std::size_t size) noexcept {
        const auto start = reinterpret_cast<std::uintptr_t>(at);
        return start >= 0x300000000ull && start <= 0x4000000000ull - size;
    }
    void* mapping{};
    unsigned char* base{};
    std::size_t bytes{};
    std::unique_ptr<unsigned char[]> fallback;
};
} // namespace Eden
