// Smoke test for logger::mmap.
// Covers the five exit-criteria from C:\Users\32765\.claude\plans\strand-mellow-cerf.md:
//   1) Region basic: create -> write -> Sync -> reopen via ifstream, bytes match.
//   2) Buffer framing: multiple variable-length writes round-trip via ForEachFrame;
//      oversize Write returns false.
//   3) Store happy path: 1k Append + Flush; PersistedBytes equals sum of payload sizes;
//      on_persist sees frames in order.
//   4) Store scaling: small buffers + 16 producer threads; PoolSize grows up to max_buffers;
//      all bytes eventually drain.
//   5) Store back-pressure: min/max=2, on_persist deliberately slow; some Append return
//      false; after slowdown lifted, Flush + total bytes accounted.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context/context.h"
#include "context/strand.h"
#include "mmap/mmap_buffer.h"
#include "mmap/mmap_region.h"
#include "mmap/mmap_store.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);\
        std::exit(1);                                                        \
    }                                                                        \
} while (0)

static fs::path MakeTempDir(const char* tag) {
    auto base = fs::temp_directory_path() / "logger_high_smoke";
    fs::create_directories(base);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s-%lld", tag,
                  static_cast<long long>(
                      std::chrono::steady_clock::now().time_since_epoch().count()));
    auto p = base / buf;
    fs::create_directories(p);
    return p;
}

static void RemoveAll(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// ---------------------------------------------------------------------------

static void test_region_basic() {
    auto dir = MakeTempDir("region");
    auto path = dir / "r.bin";
    const char kPayload[] = "hello-mmap-region-0123456789";
    const std::size_t plen = sizeof(kPayload) - 1;

    {
        auto r = logger::mmap::MmapRegion::Create(path, 4096);
        CHECK(r.size() >= 4096, "region size < requested");
        CHECK(r.data() != nullptr, "region data is null");
        std::memcpy(r.data(), kPayload, plen);
        r.Sync(0, plen);
    }  // dtor unmaps + closes

    std::ifstream in(path, std::ios::binary);
    CHECK(in.good(), "reopen region file failed");
    std::string got(plen, '\0');
    in.read(got.data(), static_cast<std::streamsize>(plen));
    CHECK(static_cast<std::size_t>(in.gcount()) == plen, "short read on region file");
    CHECK(std::memcmp(got.data(), kPayload, plen) == 0, "region content mismatch");

    RemoveAll(dir);
    std::puts("[ok] test_region_basic");
}

// ---------------------------------------------------------------------------

static void test_buffer_framing() {
    auto dir = MakeTempDir("buffer");
    auto path = dir / "b.mmap";
    logger::mmap::MmapBuffer buf(path, 1024);

    std::vector<std::string> payloads = {
        std::string(10, 'a'),
        std::string(20, 'b'),
        std::string(0,  '?'),  // empty: should be rejected
        std::string(50, 'c'),
        std::string(1,  'd'),
        std::string(100, 'e'),
    };

    std::vector<std::string> expected;
    for (const auto& p : payloads) {
        const bool ok = buf.Write(reinterpret_cast<const std::byte*>(p.data()), p.size());
        if (p.empty()) {
            CHECK(!ok, "empty Write should be rejected");
        } else {
            CHECK(ok, "small Write rejected unexpectedly");
            expected.push_back(p);
        }
    }
    // Now try to overrun: a 4000-byte payload won't fit in a 1024-cap buffer.
    std::string huge(4000, 'z');
    CHECK(!buf.Write(reinterpret_cast<const std::byte*>(huge.data()), huge.size()),
          "oversize Write should be rejected");

    buf.Seal();

    std::vector<std::string> got;
    buf.ForEachFrame([&](const std::byte* p, std::size_t n) {
        got.emplace_back(reinterpret_cast<const char*>(p), n);
    });
    CHECK(got.size() == expected.size(), "frame count mismatch");
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i] == expected[i], "frame content mismatch");
    }

    RemoveAll(dir);
    std::puts("[ok] test_buffer_framing");
}

// ---------------------------------------------------------------------------

static void test_store_happy() {
    auto dir = MakeTempDir("store_happy");
    auto& ctx = logger::context::Context::Instance();
    auto strand = ctx.MakeStrand();

    std::mutex out_mtx;
    std::vector<std::string> persisted;
    auto on_persist = [&](const std::byte* p, std::size_t n) {
        std::lock_guard<std::mutex> lk(out_mtx);
        persisted.emplace_back(reinterpret_cast<const char*>(p), n);
    };

    logger::mmap::MmapStore::Config cfg;
    cfg.dir = dir;
    cfg.buffer_bytes = 64 * 1024;
    cfg.min_buffers  = 2;
    cfg.max_buffers  = 4;
    logger::mmap::MmapStore store(cfg, strand, on_persist);

    constexpr int N = 1000;
    std::size_t expected_bytes = 0;
    for (int i = 0; i < N; ++i) {
        char tmp[32];
        const int nbytes = std::snprintf(tmp, sizeof(tmp), "payload-%04d", i);
        CHECK(nbytes > 0, "snprintf failed");
        const auto bytes = reinterpret_cast<const std::byte*>(tmp);
        CHECK(store.Append(bytes, static_cast<std::size_t>(nbytes)), "Append failed in happy path");
        expected_bytes += static_cast<std::size_t>(nbytes);
    }
    store.Flush();

    CHECK(store.PersistedBytes() == expected_bytes, "persisted bytes mismatch");
    CHECK(persisted.size() == static_cast<std::size_t>(N), "persisted frame count mismatch");

    // Frames within a single drained buffer keep producer order; across buffers,
    // order is preserved because Append serializes its swap on mtx_. Verify here.
    for (int i = 0; i < N; ++i) {
        char tmp[32];
        const int nbytes = std::snprintf(tmp, sizeof(tmp), "payload-%04d", i);
        CHECK(persisted[i] == std::string(tmp, nbytes), "frame order mismatch");
    }

    RemoveAll(dir);
    std::puts("[ok] test_store_happy");
}

// ---------------------------------------------------------------------------

static void test_store_scaling() {
    auto dir = MakeTempDir("store_scaling");
    auto& ctx = logger::context::Context::Instance();
    auto strand = ctx.MakeStrand();

    std::atomic<std::size_t> bytes_seen{0};
    std::atomic<std::size_t> frames_seen{0};
    auto on_persist = [&](const std::byte*, std::size_t n) {
        bytes_seen.fetch_add(n);
        frames_seen.fetch_add(1);
    };

    logger::mmap::MmapStore::Config cfg;
    cfg.dir = dir;
    cfg.buffer_bytes = 4 * 1024;
    cfg.min_buffers  = 2;
    cfg.max_buffers  = 4;
    cfg.back_pressure_timeout = 2s;  // generous
    logger::mmap::MmapStore store(cfg, strand, on_persist);

    constexpr int kThreads     = 16;
    constexpr int kPerThread   = 500;
    const std::string payload(64, 'x');  // 64-byte payload
    std::atomic<std::size_t> ok_count{0};

    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&]() {
            for (int i = 0; i < kPerThread; ++i) {
                if (store.Append(reinterpret_cast<const std::byte*>(payload.data()),
                                 payload.size())) {
                    ok_count.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : producers) th.join();

    const std::size_t peak_pool = store.PoolSize();
    std::fprintf(stderr, "  scaling: pool=%zu, ok_appends=%zu\n",
                 peak_pool, ok_count.load());
    CHECK(peak_pool > 2, "store did not scale beyond min_buffers");
    CHECK(peak_pool <= 4, "store scaled past max_buffers");

    store.Flush();
    CHECK(bytes_seen.load() == ok_count.load() * payload.size(),
          "scaling: total bytes mismatch");
    CHECK(frames_seen.load() == ok_count.load(), "scaling: frame count mismatch");

    RemoveAll(dir);
    std::puts("[ok] test_store_scaling");
}

// ---------------------------------------------------------------------------

static void test_store_backpressure() {
    auto dir = MakeTempDir("store_bp");
    auto& ctx = logger::context::Context::Instance();
    auto strand = ctx.MakeStrand();

    std::atomic<bool> slow{true};
    std::atomic<std::size_t> bytes_seen{0};
    std::atomic<std::size_t> frames_seen{0};
    auto on_persist = [&](const std::byte*, std::size_t n) {
        if (slow.load()) std::this_thread::sleep_for(50ms);
        bytes_seen.fetch_add(n);
        frames_seen.fetch_add(1);
    };

    logger::mmap::MmapStore::Config cfg;
    cfg.dir = dir;
    cfg.buffer_bytes = 4 * 1024;
    cfg.min_buffers  = 2;
    cfg.max_buffers  = 2;            // 不允许扩容
    cfg.back_pressure_timeout = 30ms; // 快速超时
    logger::mmap::MmapStore store(cfg, strand, on_persist);

    const std::string payload(256, 'y');
    std::atomic<std::size_t> ok{0}, drop{0};
    // 16 个线程持续 Append，预期会撞到背压上限。
    std::vector<std::thread> ths;
    for (int t = 0; t < 8; ++t) {
        ths.emplace_back([&]() {
            for (int i = 0; i < 200; ++i) {
                if (store.Append(reinterpret_cast<const std::byte*>(payload.data()),
                                 payload.size())) {
                    ok.fetch_add(1);
                } else {
                    drop.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : ths) t.join();

    std::fprintf(stderr, "  backpressure: ok=%zu drop=%zu\n",
                 ok.load(), drop.load());
    CHECK(drop.load() > 0, "back-pressure path never triggered");

    // 解除拖慢，等 drain 完成。
    slow.store(false);
    store.Flush();
    CHECK(bytes_seen.load() == ok.load() * payload.size(),
          "back-pressure: total bytes mismatch");
    CHECK(frames_seen.load() == ok.load(), "back-pressure: frame count mismatch");

    RemoveAll(dir);
    std::puts("[ok] test_store_backpressure");
}

// ---------------------------------------------------------------------------

int main() {
    test_region_basic();
    test_buffer_framing();
    test_store_happy();
    test_store_scaling();
    test_store_backpressure();

    logger::context::Context::Instance().Shutdown();

    std::puts("ALL MMAP TESTS PASSED");
    return 0;
}
