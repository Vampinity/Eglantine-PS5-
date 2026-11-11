// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "common/sparse_large_vector.h"

namespace Common {
void* AllocateHeapMemoryPages(std::size_t size) noexcept;
void* ReserveMemoryRange(std::size_t size) noexcept;
void* ReserveAddressRange(std::size_t size) noexcept;
bool CommitMemoryRange(void* address, std::size_t size) noexcept;
bool CommitMemoryRangeFrom(void* address, std::size_t size, bool high) noexcept;
int DecommitMemoryRange(void* address, std::size_t size) noexcept;
void ReleaseMemoryRange(void* address, std::size_t size) noexcept;
// The block kept for a big image (src/memory_pages.cpp, EDEN_BIG_IMAGE_RESERVE_MIB).
void TakeBigImageReserve(std::size_t mib) noexcept;
void* TakeBigImageBlock(std::size_t bytes) noexcept;
int GiveBigImageBlock(void* address) noexcept;
bool BigImageRequest(std::size_t bytes) noexcept;
} // namespace Common

namespace {
// EDEN_BIG_IMAGE_NO_SPLIT=1 (off unless set): a request of the reserve's class that cannot have
// the reserve is refused, not taken in pieces.
std::atomic<bool> big_image_no_split{false};
std::atomic<unsigned> big_image_refused{0};

std::uint64_t EnvMib(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] < '0' || value[0] > '9') return 0;
    char* end = nullptr;
    const unsigned long long mib = std::strtoull(value, &end, 10);
    return end != nullptr && *end == '\0' && mib <= 65536 ? mib : 0;
}
} // namespace

// The pool switches, read here from main right after env.txt is applied: the heap's first large
// block can come before that, and a switch read inside malloc would be read too early (and by
// getenv on the heap's own path, which must not allocate).
//   EDEN_BIG_IMAGE_RESERVE_MIB=<n>: one block of n MiB, taken now while the pool is still whole and
//     kept for a large block that finds no single free range and needs 7n/8 to n MiB (Ocarina
//     of Time's 144 MiB ground-texture decode buffer): src/memory_pages.cpp.
//   EDEN_BIG_IMAGE_NO_SPLIT=1 (needs EDEN_BIG_IMAGE_RESERVE_MIB for the class; nothing alone):
//     such a request that cannot have the reserve (given out already, or
//     the pool had no range for it at the start) fails (NULL; std::bad_alloc for C++, which the
//     texture cache's decode worker catches, and with EDEN_NX2_TC_KEEP_ASYNC tries again later)
//     instead of taking the pool's last pieces (eden_heap_split_pages): on 7 Nov (run T54, New
//     Game) that left the pool's largest free block at 2 MiB and the pipeline builder threads
//     starved. Smaller requests are not touched: they still go in pieces as before.
//   EDEN_UP_HEAP_WAIT_MS=<n> (1 to 60000; unset or 0: off): a pipeline builder thread whose request
//     the heap cannot serve waits up to n ms for memory instead of getting NULL, which the driver's
//     shader compiler does not check (headless/heap_arenas.inc, eden_heap_wait_for).
extern "C" void eden_heap_set_wait_ms(unsigned ms) __attribute__((weak));
//   EDEN_UP_HEAP_RESCUE=1: a request the heap is about to refuse is asked of every arena first
//     (headless/heap_arenas.inc, eden_heap_arena_rescue).
extern "C" void eden_heap_set_rescue(int on) __attribute__((weak));
//   EDEN_TEST_HEAP_STARVE=<every>[,<attempts>] (a test): every <every>-th request of 256 KiB or more
//     on a pipeline builder thread is refused, <attempts> times in a row (default 150), with no
//     memory taken away (headless/heap_arenas.inc, eden_heap_starve).
extern "C" void eden_heap_set_starve(unsigned every, unsigned attempts) __attribute__((weak));
//   EDEN_UP_HEAP_WAIT_GPU_MS=<n> (1 to 5000; unset or 0: off): the same wait for a thread that builds a
//     pipeline itself at run time (the GPU thread at a pipeline's first sight), which holds the frame:
//     its own limit, and at most n ms a build (headless/heap_arenas.inc, eden_heap_wait_scope).
extern "C" void eden_heap_set_wait_gpu_ms(unsigned ms) __attribute__((weak));
//   EDEN_TEST_HEAP_STARVE_GPU=1 (a test, with EDEN_TEST_HEAP_STARVE): the refusals go to the threads
//     inside such a run-time build instead of the builder threads.
extern "C" void eden_heap_set_starve_scope(int on) __attribute__((weak));
extern "C" void eden_heap_set_large(std::size_t kib) __attribute__((weak));
extern "C" void eden_heap_pool_init(void) {
    // EDEN_HEAP_LARGE_MIB=<n> (1 to 32) or EDEN_HEAP_LARGE_KIB=<n> (256 to 32768): requests of that
    // size or more get memory of their own, given back when they are freed
    // (headless/heap_arenas.inc). Without either 32 MiB, as before.
    if (eden_heap_set_large) {
        std::uint64_t kib = EnvMib("EDEN_HEAP_LARGE_KIB");
        if (kib == 0) kib = EnvMib("EDEN_HEAP_LARGE_MIB") << 10;
        if (kib != 0) {
            kib = kib < 256 ? 256 : kib > 32768 ? 32768 : kib;
            eden_heap_set_large(static_cast<std::size_t>(kib));
            std::fprintf(stderr, "EDEN_HEAP_LARGE requests of %llu KiB or more get memory of their own\n",
                         static_cast<unsigned long long>(kib));
        }
    }
    if (const char* const starve = std::getenv("EDEN_TEST_HEAP_STARVE");
        starve != nullptr && starve[0] >= '1' && starve[0] <= '9' && eden_heap_set_starve) {
        char* end = nullptr;
        const unsigned long every = std::strtoul(starve, &end, 10);
        unsigned long attempts = 0;
        if (end != nullptr && *end == ',')
            attempts = std::strtoul(end + 1, &end, 10);
        if (end != nullptr && *end == '\0' && every <= 1000000 && attempts <= 100000) {
            eden_heap_set_starve(static_cast<unsigned>(every), static_cast<unsigned>(attempts));
            std::fprintf(stderr, "EDEN_TEST_HEAP_STARVE every %lu-th request of 256 KiB or more on a pipeline builder "
                         "thread is refused, %lu times in a row\n", every, attempts != 0 ? attempts : 150UL);
        }
    }
    if (const char* const wait = std::getenv("EDEN_UP_HEAP_WAIT_MS"); wait != nullptr && wait[0] >= '0' && wait[0] <= '9') {
        char* end = nullptr;
        const unsigned long ms = std::strtoul(wait, &end, 10);
        if (end != nullptr && *end == '\0' && ms != 0 && ms <= 60000 && eden_heap_set_wait_ms) {
            eden_heap_set_wait_ms(static_cast<unsigned>(ms));
            std::fprintf(stderr, "EDEN_UP_HEAP_WAIT_MS pipeline builders wait up to %lu ms for memory\n", ms);
        }
    }
    if (const char* const wait = std::getenv("EDEN_UP_HEAP_WAIT_GPU_MS"); wait != nullptr && wait[0] >= '0' && wait[0] <= '9') {
        char* end = nullptr;
        const unsigned long ms = std::strtoul(wait, &end, 10);
        if (end != nullptr && *end == '\0' && ms != 0 && ms <= 5000 && eden_heap_set_wait_gpu_ms) {
            eden_heap_set_wait_gpu_ms(static_cast<unsigned>(ms));
            std::fprintf(stderr, "EDEN_UP_HEAP_WAIT_GPU_MS a run-time pipeline build waits up to %lu ms for memory\n", ms);
        }
    }
    if (const char* const scope = std::getenv("EDEN_TEST_HEAP_STARVE_GPU");
        scope != nullptr && scope[0] == '1' && scope[1] == '\0' && eden_heap_set_starve_scope) {
        eden_heap_set_starve_scope(1);
        std::fputs("EDEN_TEST_HEAP_STARVE_GPU the starve test refuses requests inside run-time pipeline builds\n", stderr);
    }
    if (const char* const rescue = std::getenv("EDEN_UP_HEAP_RESCUE");
        rescue != nullptr && rescue[0] == '1' && rescue[1] == '\0' && eden_heap_set_rescue) {
        eden_heap_set_rescue(1);
        std::fputs("EDEN_UP_HEAP_RESCUE a request about to be refused is asked of every arena first\n", stderr);
    }
    if (const std::uint64_t mib = EnvMib("EDEN_BIG_IMAGE_RESERVE_MIB"); mib != 0)
        Common::TakeBigImageReserve(static_cast<std::size_t>(mib));
    // EDEN_TEST_POOL_HOG_MIB=<n> (a test, off unless set): n MiB (in 32 MiB pieces) taken out of the pool
    // now and never given back, so this console runs out where one with less free memory does (the
    // door of Link's house on a PS5 Pro on 13.x, issue 21) and a memory fix can be tried in a 3 minute run.
    if (const std::uint64_t hog = EnvMib("EDEN_TEST_POOL_HOG_MIB"); hog != 0) {
        constexpr std::size_t piece = std::size_t{32} << 20;
        std::uint64_t taken = 0;
        for (; taken + 32 <= hog; taken += 32) {
            void* const address = Common::ReserveAddressRange(piece);
            if (address == nullptr || !Common::CommitMemoryRange(address, piece)) break;
        }
        std::fprintf(stderr, "EDEN_TEST_POOL_HOG %llu MiB of the pool taken for good (asked %llu)\n",
                     static_cast<unsigned long long>(taken), static_cast<unsigned long long>(hog));
    }
    const char* const no_split = std::getenv("EDEN_BIG_IMAGE_NO_SPLIT");
    big_image_no_split.store(no_split != nullptr && no_split[0] == '1', std::memory_order_relaxed);
    if (big_image_no_split.load(std::memory_order_relaxed))
        std::fputs("EDEN_BIG_IMAGE_NO_SPLIT a big image's block is refused when the reserve cannot serve it\n", stderr);
}

// 1: a large block of this size, of the reserve's class, must not be taken in pieces (the request
// fails instead); 0: as before. Inside malloc: no stdio.
extern "C" int eden_heap_big_image_refuses(std::size_t size) {
    if (!big_image_no_split.load(std::memory_order_relaxed) || !Common::BigImageRequest(size)) return 0;
    big_image_refused.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// How often such a block was refused (the memory reports, when they want it).
extern "C" unsigned eden_heap_big_image_refused(void) {
    return big_image_refused.load(std::memory_order_relaxed);
}

// The big-image reserve for a large block, or NULL (headless/heap_arenas.inc).
extern "C" void* eden_heap_reserve_pages(std::size_t size) {
    return Common::TakeBigImageBlock(size);
}
extern "C" void eden_heap_reserve_pages_free(void* base, std::size_t size) {
    (void)size;
    (void)Common::GiveBigImageBlock(base);
}

// Reuse the qualified, owned direct-memory backend for the C mspace heap.
extern "C" void* eden_heap_pages(std::size_t size) {
    return Common::AllocateHeapMemoryPages(size);
}
extern "C" void eden_heap_pages_free(void* base, std::size_t size) {
    Common::FreeMemoryPages(base, size);
}
// The heap that grows (headless/heap_arenas.inc): address space first, memory piece by piece.
extern "C" void* eden_heap_reserve(std::size_t size) {
    return Common::ReserveMemoryRange(size);
}
extern "C" int eden_heap_commit(void* address, std::size_t size) {
    return Common::CommitMemoryRange(address, size) ? 0 : -1;
}
// A piece that holds nothing goes back to the system; its address space stays the heap's.
// 1: given back, 0: nothing done, -1: given back in part (not to be used again).
extern "C" int eden_heap_decommit(void* address, std::size_t size) {
    return Common::DecommitMemoryRange(address, size);
}
// A large block whose memory the pool has in no single free block: address space of its own,
// with memory behind it in blocks of whatever size the pool has (zeroed), given back whole.
extern "C" void* eden_heap_split_pages(std::size_t size) {
    constexpr std::size_t large_page = std::size_t{2} << 20;
    if (size == 0 || size > ~std::size_t{0} - large_page) return nullptr;
    const std::size_t span = (size + large_page - 1) / large_page * large_page;
    void* address = Common::ReserveAddressRange(span);
    if (address == nullptr) return nullptr;
    if (!Common::CommitMemoryRangeFrom(address, span, false)) {
        Common::ReleaseMemoryRange(address, span);
        return nullptr;
    }
    return address;
}
extern "C" void eden_heap_split_pages_free(void* address, std::size_t size) {
    constexpr std::size_t large_page = std::size_t{2} << 20;
    Common::ReleaseMemoryRange(address, (size + large_page - 1) / large_page * large_page);
}
