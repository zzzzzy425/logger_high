// Smoke test for logger::context.
// Verifies four properties from the plan's exit-criteria:
//   1) ThreadPool: 1k Submit tasks all complete; WorkerCount goes to 0 after Shutdown.
//   2) Dynamic scaling: blocking tasks make WorkerCount climb to max; after idle_timeout
//      it falls back to min_threads.
//   3) Strand serializes: 1k increments + thread-id capture; counter == 1k and the
//      strand sees only one thread id at a time (no overlap).
//   4) Drain on Shutdown: tasks posted right before Shutdown all run.
//
// Failures abort with a non-zero exit code.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "context/context.h"
#include "context/strand.h"
#include "context/thread_pool.h"

using namespace std::chrono_literals;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
        std::exit(1);                                                    \
    }                                                                    \
} while (0)

static void test_submit_1k() {
    using logger::context::ThreadPool;
    ThreadPool::Config cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 8;
    cfg.idle_timeout = 500ms;
    ThreadPool pool(cfg);

    constexpr int N = 1000;
    std::vector<std::future<int>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(pool.Submit([i]() { return i * 2; }));
    }
    long long sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += futs[i].get();
    }
    CHECK(sum == 1LL * (N - 1) * N, "submit 1k sum mismatch");

    pool.Shutdown();
    CHECK(pool.WorkerCount() == 0, "WorkerCount != 0 after Shutdown");
    std::puts("[ok] test_submit_1k");
}

static void test_scaling() {
    using logger::context::ThreadPool;
    ThreadPool::Config cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 6;
    cfg.scale_factor = 1;
    cfg.idle_timeout = 300ms;
    ThreadPool pool(cfg);

    CHECK(pool.WorkerCount() == 2, "initial worker count != min_threads");

    // Fill with blocking tasks; this should drive scale-out to max_threads.
    std::atomic<int> running{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 20; ++i) {
        futs.push_back(pool.Submit([&]() {
            running.fetch_add(1);
            std::this_thread::sleep_for(200ms);
            running.fetch_sub(1);
        }));
    }

    // Give the scaler some time to kick in.
    std::this_thread::sleep_for(150ms);
    const size_t peak = pool.WorkerCount();
    std::fprintf(stderr, "  peak worker count: %zu\n", peak);
    CHECK(peak > 2,  "did not scale up beyond min_threads");
    CHECK(peak <= 6, "scaled past max_threads");

    for (auto& f : futs) f.get();

    // Wait long enough for idle workers to retire.
    std::this_thread::sleep_for(800ms);
    const size_t settled = pool.WorkerCount();
    std::fprintf(stderr, "  settled worker count: %zu\n", settled);
    CHECK(settled == 2, "worker count did not shrink back to min_threads");

    pool.Shutdown();
    std::puts("[ok] test_scaling");
}

static void test_strand_serial() {
    using logger::context::ThreadPool;
    using logger::context::Strand;
    ThreadPool pool(ThreadPool::Config{2, 8, 2, 500ms});

    auto strand = Strand::Create(pool);

    constexpr int N = 1000;
    // Use plain int + bool flag to detect overlap. If strand truly serializes,
    // no two tasks run at the same time.
    int counter = 0;
    std::atomic<bool> inside{false};
    std::atomic<bool> overlap_seen{false};
    std::set<std::thread::id> seen_threads;
    std::mutex seen_mtx;

    std::vector<std::future<void>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(strand->Submit([&]() {
            if (inside.exchange(true)) overlap_seen.store(true);
            ++counter;
            {
                std::lock_guard<std::mutex> lk(seen_mtx);
                seen_threads.insert(std::this_thread::get_id());
            }
            inside.store(false);
        }));
    }
    for (auto& f : futs) f.get();

    CHECK(!overlap_seen.load(), "strand allowed overlapping execution");
    CHECK(counter == N, "strand counter lost increments");
    std::fprintf(stderr, "  strand ran across %zu distinct threads (any number is fine)\n",
                 seen_threads.size());

    pool.Shutdown();
    std::puts("[ok] test_strand_serial");
}

static void test_drain_on_shutdown() {
    using logger::context::ThreadPool;
    ThreadPool pool(ThreadPool::Config{2, 4, 2, 500ms});

    constexpr int N = 200;
    std::atomic<int> done{0};
    for (int i = 0; i < N; ++i) {
        pool.PostDetached([&]() {
            std::this_thread::sleep_for(2ms);
            done.fetch_add(1);
        });
    }
    pool.Shutdown();
    CHECK(done.load() == N, "Shutdown did not drain queued tasks");
    std::puts("[ok] test_drain_on_shutdown");
}

static void test_context_singleton() {
    using logger::context::Context;
    auto& ctx = Context::Instance();
    ctx.Configure(logger::context::ThreadPool::Config{2, 4, 2, 500ms});

    std::atomic<int> count{0};
    auto strand = ctx.MakeStrand();
    constexpr int N = 100;
    std::vector<std::future<void>> futs;
    for (int i = 0; i < N; ++i) {
        futs.push_back(strand->Submit([&]() { count.fetch_add(1); }));
    }
    for (auto& f : futs) f.get();
    CHECK(count.load() == N, "context singleton strand lost increments");

    ctx.Shutdown();
    std::puts("[ok] test_context_singleton");
}

int main() {
    test_submit_1k();
    test_scaling();
    test_strand_serial();
    test_drain_on_shutdown();
    test_context_singleton();
    std::puts("ALL CONTEXT TESTS PASSED");
    return 0;
}
