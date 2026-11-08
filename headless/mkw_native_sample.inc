// Native stack sampler. Started as a temporary MKW diagnostic; the named-thread windows below follow
// GrowingBlackBeans' frontend-stack-sampler (viejunerd issue 21), written again for this tree.
//
// A helper thread sends SIGUSR2 to one worker thread every 5 ms. That thread's handler copies its
// pc, up to kStackWords words of its stack and its frame-pointer chain into a fixed array: no
// allocation, no locks, no logging in the handler. A window's samples go to
// /data/eglantineps5/logs/sample-<pid>-<window>-<thread>.bin (header below; summarized by
// console/eglantine-tools/sample-report.py with the build's llvm-pie.elf).
//
// Windows start in two ways:
// - EDEN_SAMPLE_THREADS=GPUBack,GPU (names from kWorkerNames). Unset or empty: no planned windows,
//   as before. EDEN_SAMPLE_START_S (default 90) counts from the first GPU or GPUBack thread, so from
//   the game's boot; EDEN_SAMPLE_COUNT is the samples per window (default 6000, about 30 s);
//   EDEN_SAMPLE_ROUNDS (default 2): each listed thread gets one window per round, in turn.
// - logs/mkw-profile-arm.txt "sequence target count", as before (target: index into kWorkerNames).
//   Its windows are now written in the same format and under the same name as the planned ones
//   (they were mkw-sample-<pid>-<target>.bin with a 5-word header).
// Build with EDEN_SAMPLER_FRAME_POINTERS=ON for whole call chains in video_core.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>
#include <pthread_np.h>
#include <unistd.h>
extern "C" {
// The app's code, from the link (tools/unwind.ld): the summarizer maps pc values into the ELF.
extern const char __eden_text_start[];
extern const char __eden_text_end[];
}
namespace MkwNativeSample {
struct Block { std::atomic<unsigned long long> entry{}, size{}, location{}; };
constexpr unsigned kStackWords = 64, kFrames = 24;
struct Sample {
    std::atomic<unsigned long long> pc{}, highwater{}, rsp{}, rbp{}, marker{};
    std::array<std::atomic<unsigned long long>, kStackWords> stack{};
    std::array<std::atomic<unsigned long long>, kFrames> frames{};
};
static_assert(sizeof(Sample) == 8 * (5 + kStackWords + kFrames));
// File header: magic "MKWSAMP2", pid, target, samples, sizeof(Sample), code start, code end,
// signals sent, window ms, stack low, stack high, kStackWords, kFrames (13 little-endian u64).
constexpr unsigned long long kMagic = 0x4d4b5753414d5032ULL;
inline std::array<Block, 1 << 20> blocks{};
inline std::atomic<unsigned long long> block_count{}, block_overflow{};
inline std::array<Sample, 8192> samples{};  // 6 MiB, a little less than the 16384 x 392 bytes before
inline std::atomic<unsigned long long> sample_count{}, state{};
inline uintptr_t stack_low{}, stack_high{};
struct Worker { std::atomic<pthread_t> thread{}; uintptr_t low{}, high{}; };
// 0-5 CPUCore_N, 6 GPU (the split's front), 7 GPUBack (the split's back).
constexpr const char* kWorkerNames[] = {"CPUCore_0", "CPUCore_1", "CPUCore_2", "CPUCore_3",
                                        "CPUCore_4", "CPUCore_5", "GPU",       "GPUBack"};
constexpr unsigned kWorkerCount = sizeof(kWorkerNames) / sizeof(kWorkerNames[0]);
inline std::array<Worker, kWorkerCount> workers{};
// The worker being sampled; kWorkerCount: none, and the handler ignores the signal.
inline std::atomic<unsigned long long> sample_target{kWorkerCount};
inline std::atomic<long long> boot_ms{0};  // the first GPU or GPUBack thread started
struct FiberStack { std::atomic<uintptr_t> low{}, high{}; };
inline std::array<FiberStack, 2048> fiber_stacks{};
inline std::mutex fiber_stacks_mutex;
inline std::atomic<unsigned long long> fiber_stack_overflow{};
inline long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
void RecordFiberStack(void* base, size_t bytes) {
    const auto low = reinterpret_cast<uintptr_t>(base);
    std::lock_guard lock{fiber_stacks_mutex};
    if (!bytes) {
        for (auto& stack : fiber_stacks) {
            if (stack.low.load() == low) {
                stack.low.store(0, std::memory_order_release);
                return;
            }
        }
        return;
    }
    for (auto& stack : fiber_stacks) {
        if (!stack.low.load()) {
            stack.high.store(low + bytes, std::memory_order_relaxed);
            stack.low.store(low, std::memory_order_release);
            return;
        }
    }
    fiber_stack_overflow.fetch_add(1, std::memory_order_relaxed);
}

static_assert(std::atomic<unsigned long long>::is_always_lock_free);
void Signal(int, siginfo_t*, void* context) {
    // Only the thread being sampled answers: a stray SIGUSR2, or one that arrives after its window
    // closed, leaves the array alone.
    const auto target = sample_target.load(std::memory_order_acquire);
    if (target >= workers.size() ||
        !pthread_equal(pthread_self(), workers[target].thread.load(std::memory_order_acquire))) return;
    const auto i = sample_count.load(std::memory_order_relaxed);
    if (i >= samples.size()) return;
    auto& sample = samples[i];
    // Read only; validate sampled addresses against this run's executable maps.
    sample.highwater.store(block_count.load(std::memory_order_relaxed));
    const auto* words = static_cast<const uintptr_t*>(context);
    const auto pc = words[224 / sizeof(uintptr_t)];
    sample.pc.store(pc);
    const auto rsp = words[248 / sizeof(uintptr_t)];
    sample.rsp.store(rsp); sample.rbp.store(words[136 / sizeof(uintptr_t)]);
    char marker;
    const auto here = reinterpret_cast<uintptr_t>(&marker);
    sample.marker.store(here);
    // The stack the interrupted code ran on: the thread's own, or (guest kernel contexts) a
    // Common::Fiber allocation with its registered live bounds. Unknown: low = high = 0.
    auto low = stack_low, high = stack_high;
    if (rsp < low || rsp >= high) {
        low = high = 0;
        for (const auto& stack : fiber_stacks) {
            const auto begin = stack.low.load(std::memory_order_acquire);
            const auto end = stack.high.load(std::memory_order_relaxed);
            if (begin && stack.low.load(std::memory_order_acquire) == begin &&
                rsp >= begin && rsp < end) { low = begin; high = end; break; }
        }
    }
    for (auto& word : sample.stack) word.store(0, std::memory_order_relaxed);
    // The handler runs on the interrupted stack, below rsp (anything else is rejected). Up to
    // kStackWords words: to the end of a known stack, otherwise only to the end of rsp's page.
    if (rsp > here && rsp - here < 65536 && (rsp & 7) == 0) {
        std::size_t count = (4096 - (rsp & 4095)) / sizeof(uintptr_t);
        if (high && rsp >= low && rsp < high) count = (high - rsp) / sizeof(uintptr_t);
        if (count > kStackWords) count = kStackWords;
        const auto* stack = reinterpret_cast<const uintptr_t*>(rsp);
        for (std::size_t j = 0; j < count; ++j) sample.stack[j].store(stack[j], std::memory_order_relaxed);
    }
    for (auto& frame : sample.frames) frame.store(0, std::memory_order_relaxed);
    auto frame = words[136 / sizeof(uintptr_t)];
    for (unsigned depth = 0; depth < sample.frames.size(); ++depth) {
        if ((frame & 7) || frame < rsp || frame < low || high < 16 ||
            frame > high - 16) break;
        const auto* record = reinterpret_cast<const uintptr_t*>(frame);
        const auto next = record[0];
        sample.frames[depth].store(record[1], std::memory_order_relaxed);
        if (next <= frame || next - frame > 128 * 1024) break;
        frame = next;
    }
    sample_count.store(i + 1, std::memory_order_release);
}

// One window: up to `count` samples of worker `target`, 5 ms apart, then the file. False when the
// thread could not be signalled (the sampler stops for this process, as before).
inline bool SampleWindow(unsigned target, unsigned count, unsigned window) {
    const auto thread = workers[target].thread.load(std::memory_order_acquire);
    if (!thread) {
        std::fprintf(stderr, "EDEN_SAMPLE window=%u thread=%s not_running\n", window, kWorkerNames[target]);
        return true;
    }
    stack_low = workers[target].low;
    stack_high = workers[target].high;
    sample_count.store(0, std::memory_order_release);
    state.store(2, std::memory_order_release);
    sample_target.store(target, std::memory_order_release);  // publishes the bounds above too
    const unsigned limit = std::clamp<unsigned>(count, 200U, static_cast<unsigned>(samples.size()));
    std::fprintf(stderr, "EDEN_SAMPLE window=%u thread=%s start count=%u\n", window, kWorkerNames[target], limit);
    const auto started = NowMs();
    unsigned sent = 0;
    bool failed = false;
    for (; sent < limit; ++sent) {
        if (pthread_kill(thread, SIGUSR2)) { failed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the last answer
    sample_target.store(kWorkerCount, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));   // a handler already past the check
    const auto elapsed = NowMs() - started;
    const auto taken = std::min<unsigned long long>(sample_count.load(std::memory_order_acquire), samples.size());
    const unsigned long long header[]{kMagic, static_cast<unsigned long long>(getpid()), target, taken,
        sizeof(Sample), reinterpret_cast<uintptr_t>(__eden_text_start), reinterpret_cast<uintptr_t>(__eden_text_end),
        sent, static_cast<unsigned long long>(elapsed), stack_low, stack_high, kStackWords, kFrames};
    static_assert(sizeof(header) == 13 * 8);
    char path[200];
    std::snprintf(path, sizeof(path), "/data/eglantineps5/logs/sample-%u-%u-%s.bin", unsigned(getpid()), window,
                  kWorkerNames[target]);
    bool written = false;
    if (auto* file = std::fopen(path, "wb")) {
        written = std::fwrite(header, sizeof(header), 1, file) == 1 &&
                  std::fwrite(samples.data(), sizeof(Sample), taken, file) == taken;
        written = std::fclose(file) == 0 && written;
    }
    std::fprintf(stderr, "EDEN_SAMPLE window=%u thread=%s done signals=%u samples=%llu ms=%lld file=%s%s%s\n",
                 window, kWorkerNames[target], sent, taken, elapsed, path, written ? "" : " not_written",
                 failed ? " signal_failed" : "");
    state.store(failed ? 93 : 3, std::memory_order_release);
    return !failed;
}

// EDEN_SAMPLE_THREADS, read once: the workers to sample in turn, as indices into kWorkerNames.
inline std::vector<unsigned> PlannedTargets() {
    std::vector<unsigned> targets;
    const char* list = std::getenv("EDEN_SAMPLE_THREADS");
    if (!list || !*list) return targets;
    const std::string text{list};
    for (std::size_t at = 0; at <= text.size();) {
        const auto end = std::min(text.find(',', at), text.size());
        const auto name = text.substr(at, end - at);
        bool known = false;
        for (unsigned i = 0; i < kWorkerCount; ++i)
            if (name == kWorkerNames[i]) { targets.push_back(i); known = true; }
        if (!known && !name.empty())
            std::fprintf(stderr, "EDEN_SAMPLE unknown thread=%s (CPUCore_0..5, GPU, GPUBack)\n", name.c_str());
        at = end + 1;
    }
    return targets;
}
inline unsigned EnvNumber(const char* name, unsigned fallback) {
    const char* text = std::getenv(name);
    return text && *text ? static_cast<unsigned>(std::strtoul(text, nullptr, 10)) : fallback;
}

void Register(const char* name) {
    unsigned index = kWorkerCount;
    for (unsigned i = 0; i < kWorkerCount; ++i)
        if (!std::strcmp(name, kWorkerNames[i])) index = i;
    if (index >= workers.size()) return;
    auto& worker = workers[index];
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) == 0) {
        if (pthread_attr_get_np(pthread_self(), &attributes) == 0) {
            void* base{};
            size_t bytes{};
            if (pthread_attr_getstack(&attributes, &base, &bytes) == 0) {
                worker.low = reinterpret_cast<uintptr_t>(base);
                worker.high = worker.low + bytes;
            }
        }
        pthread_attr_destroy(&attributes);
    }
    worker.thread.store(pthread_self(), std::memory_order_release);
    if (index >= 6) {
        long long none = 0;
        boot_ms.compare_exchange_strong(none, NowMs(), std::memory_order_acq_rel);
    }
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
    if (pthread_sigmask(SIG_UNBLOCK, &mask, nullptr)) return;
    unsigned long long expected = 0;
    if (!state.compare_exchange_strong(expected, 1)) return;
    struct sigaction old{}, action{};
    if (sigaction(SIGUSR2, nullptr, &old) || old.sa_handler != SIG_DFL) { state.store(90); return; }
    action.sa_sigaction = Signal; action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, nullptr)) { state.store(91); return; }
    std::thread([] {
        // Read once, here: the first worker starts with the game, after env.txt was applied.
        const auto planned = PlannedTargets();
        const unsigned start_s = EnvNumber("EDEN_SAMPLE_START_S", 90);
        const unsigned count = EnvNumber("EDEN_SAMPLE_COUNT", 6000);
        const unsigned rounds = EnvNumber("EDEN_SAMPLE_ROUNDS", 2);
        if (!planned.empty())
            std::fprintf(stderr, "EDEN_SAMPLE planned threads=%zu start_s=%u count=%u rounds=%u\n", planned.size(),
                         start_s, count, rounds);
        bool plan_pending = !planned.empty();
        unsigned window = 0;
        struct Arm { unsigned long long sequence{}; unsigned target{6}, count{12000}; };
        const auto read_arm = [] {
            Arm arm;
            if (auto* file = std::fopen("/data/eglantineps5/logs/mkw-profile-arm.txt", "r")) {
                (void)std::fscanf(file, "%llu %u %u", &arm.sequence, &arm.target, &arm.count);
                std::fclose(file);
            }
            return arm;
        };
        auto previous = read_arm().sequence;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const auto boot = boot_ms.load(std::memory_order_acquire);
            if (plan_pending && boot && NowMs() - boot >= static_cast<long long>(start_s) * 1000) {
                plan_pending = false;
                bool ok = true;
                for (unsigned round = 0; round < rounds && ok; ++round)
                    for (const unsigned target : planned)
                        if (ok) ok = SampleWindow(target, count, window++);
                std::fprintf(stderr, "EDEN_SAMPLE plan finished windows=%u ok=%d\n", window, ok ? 1 : 0);
                if (!ok) return;
            }
            const auto arm = read_arm();
            if (!arm.sequence || arm.sequence == previous) continue;
            previous = arm.sequence;
            if (arm.target >= workers.size()) continue;
            if (!SampleWindow(arm.target, arm.count, window++)) return;
        }
    }).detach();
}
}
extern "C" void mkw_native_block(unsigned long long location, const void* entry, unsigned long long size) {
    const auto i = MkwNativeSample::block_count.fetch_add(1);
    if (i >= MkwNativeSample::blocks.size()) { MkwNativeSample::block_overflow.fetch_add(1); return; }
    auto& b = MkwNativeSample::blocks[i]; b.size.store(size); b.location.store(location);
    b.entry.store(reinterpret_cast<uintptr_t>(entry), std::memory_order_release);
}

extern "C" void mkw_native_fiber_stack(void* base, size_t bytes) {
    MkwNativeSample::RecordFiberStack(base, bytes);
}
