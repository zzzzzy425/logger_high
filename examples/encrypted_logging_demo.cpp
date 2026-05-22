// encrypted_logging_demo — write 100 encrypted log records to D:/logger_high/demo_logs
// and print step-by-step instructions for how to decrypt them.
//
// Output directory is fixed (not a temp dir) so you can find it by hand. The
// program pauses at the end so you can read the output if you double-click it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <cryptopp/osrng.h>
#include <cryptopp/secblock.h>
#include <cryptopp/xed25519.h>

#include "audit/shard_reader.h"
#include "context/context.h"
#include "context/strand.h"
#include "handle/log_handle.h"
#include "handle/log_macros.h"
#include "sinks/effective_sink.h"
#include "sinks/effective_sink_config.h"
#include "utils/log_level.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr const char* kDemoRoot = "D:/logger_high/demo_logs";
constexpr int         kNumRecords = 200000;

struct AuditKeypair {
    CryptoPP::SecByteBlock priv{32};
    CryptoPP::SecByteBlock pub{32};
};

AuditKeypair MakeAuditKeypair() {
    AuditKeypair kp;
    CryptoPP::AutoSeededRandomPool prng;
    CryptoPP::x25519               dh;
    dh.GeneratePrivateKey(prng, kp.priv.BytePtr());
    dh.GeneratePublicKey(prng, kp.priv.BytePtr(), kp.pub.BytePtr());
    return kp;
}

void WriteFileBytes(const fs::path& p, const unsigned char* data, std::size_t n) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        throw std::runtime_error("cannot create " + p.string());
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
}

// Build a unique subdir like "D:/logger_high/demo_logs/run-20260522-221911-720".
// Using a fresh subdir per invocation avoids any clash with a prior demo run
// that may still hold mmap buffer files open or have stale shard files behind.
fs::path MakeRunDir() {
    fs::create_directories(kDemoRoot);
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t   = clock::to_time_t(now);
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()).count() % 1000;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "run-%04d%02d%02d-%02d%02d%02d-%03lld",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  static_cast<long long>(ms));
    fs::path dir = fs::path(kDemoRoot) / buf;
    fs::create_directories(dir);
    return dir;
}

std::string LevelName(std::uint32_t v) {
    return std::string(::logger::ToString(static_cast<::logger::LogLevel>(v)));
}

}  // namespace

int main() {
    try {
        const fs::path demo_dir = MakeRunDir();

        std::printf("=== encrypted_logging_demo ===\n");
        std::printf("Output directory: %s\n\n", demo_dir.string().c_str());

        // 1. Generate audit keypair and write both halves to disk.
        const auto kp = MakeAuditKeypair();
        const fs::path pub_path  = demo_dir / "server.pub";
        const fs::path priv_path = demo_dir / "server.priv";
        WriteFileBytes(pub_path,  kp.pub.BytePtr(),  kp.pub.SizeInBytes());
        WriteFileBytes(priv_path, kp.priv.BytePtr(), kp.priv.SizeInBytes());
        std::printf("[1/4] wrote audit keypair:\n");
        std::printf("        public  : %s   (used by the logger at runtime)\n",
                    pub_path.string().c_str());
        std::printf("        private : %s   (used offline by log_decoder to recover plaintext)\n\n",
                    priv_path.string().c_str());

        // 2. Spin up the logging pipeline.
        auto& ctx = logger::context::Context::Instance();
        logger::context::ThreadPool::Config tp_cfg;
        tp_cfg.min_threads = 2;
        tp_cfg.max_threads = 4;
        ctx.Configure(tp_cfg);

        logger::sinks::EffectiveSinkConfig cfg;
        cfg.log_dir              = demo_dir;
        cfg.server_pub_key_path  = pub_path;
        cfg.shard_size_bytes     = 4ull * 1024 * 1024;   // 4 MiB per shard — multiple shards
        cfg.dir_size_cap         = 512ull * 1024 * 1024; // 512 MiB cap
        cfg.eviction_interval    = 60s;
        cfg.mmap_buffer_bytes    = 1ull * 1024 * 1024;   // 1 MiB per mmap cache buffer
        cfg.mmap_min_buffers     = 2;
        cfg.mmap_max_buffers     = 8;
        cfg.back_pressure_timeout = 1000ms;
        cfg.zlib_level           = 6;

        auto strand    = ctx.MakeStrand();
        auto sink_uniq = logger::sinks::EffectiveSink::Create(cfg, strand);
        std::shared_ptr<logger::sinks::LogSink> sink = std::move(sink_uniq);

        // 3. Write kNumRecords records via the LOG_* macros (the real user-facing API).
        double elapsed_sec = 0.0;
        {
            logger::LogHandle handle("demo", sink);
            handle.SetLevel(logger::LogLevel::trace);

            std::printf("[2/4] writing %d records through LOG_INFO/LOG_WARN/LOG_ERROR macros...\n",
                        kNumRecords);

            // Pre-build bodies so the loop is dominated by the logging pipeline,
            // not by string ops.
            std::vector<std::string> bodies;
            bodies.reserve(kNumRecords);
            for (int i = 0; i < kNumRecords; ++i) {
                bodies.push_back("demo record #" + std::to_string(i) +
                                 " — payload bytes get proto-serialized, zlib-compressed, AES-GCM-encrypted");
            }

            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kNumRecords; ++i) {
                const auto& m = bodies[i];
                switch (i % 3) {
                    case 0: LOG_INFO(handle,  m); break;
                    case 1: LOG_WARN(handle,  m); break;
                    case 2: LOG_ERROR(handle, m); break;
                }
            }
            handle.Flush();
            const auto t1 = std::chrono::steady_clock::now();
            elapsed_sec = std::chrono::duration<double>(t1 - t0).count();

            const double rps = elapsed_sec > 0.0 ? kNumRecords / elapsed_sec : 0.0;
            std::printf("        done in %.3f s  (%.0f records/sec, Debug build)\n\n",
                        elapsed_sec, rps);
        }
        sink.reset();  // tear sink down so all background threads stop

        // 4. List the resulting files: BOTH the final encrypted shards AND the
        //    mmap cache buffers that backed the write path.
        std::vector<fs::path> shards;
        std::vector<fs::path> caches;
        for (const auto& de : fs::directory_iterator(demo_dir)) {
            if (!de.is_regular_file()) continue;
            const auto name = de.path().filename().string();
            if (name.size() > 8 &&
                name.compare(0, 4, "log-") == 0 &&
                name.compare(name.size() - 4, 4, ".bin") == 0) {
                shards.push_back(de.path());
            } else if (name.size() > 12 &&
                       name.compare(0, 7, "buffer-") == 0 &&
                       name.compare(name.size() - 5, 5, ".mmap") == 0) {
                caches.push_back(de.path());
            }
        }
        std::sort(shards.begin(), shards.end());
        std::sort(caches.begin(), caches.end());

        std::uintmax_t shard_total = 0;
        std::printf("[3/4] files on disk:\n");
        std::printf("  shard files (log-*.bin) — final encrypted archive, AES-256-GCM ciphertext:\n");
        for (const auto& p : shards) {
            std::error_code ec;
            const auto sz = fs::file_size(p, ec);
            shard_total += sz;
            std::printf("        %s   (%llu bytes)\n",
                        p.string().c_str(),
                        static_cast<unsigned long long>(sz));
        }
        std::printf("        ---- subtotal: %llu bytes across %zu shard(s)\n",
                    static_cast<unsigned long long>(shard_total), shards.size());

        std::uintmax_t cache_total = 0;
        std::printf("  cache files (buffer-*.mmap) — mmap-backed ring buffer, reused while writing:\n");
        for (const auto& p : caches) {
            std::error_code ec;
            const auto sz = fs::file_size(p, ec);
            cache_total += sz;
            std::printf("        %s   (%llu bytes)\n",
                        p.string().c_str(),
                        static_cast<unsigned long long>(sz));
        }
        std::printf("        ---- subtotal: %llu bytes across %zu cache buffer(s)\n",
                    static_cast<unsigned long long>(cache_total), caches.size());
        std::printf("\n");
        std::printf("  How the two relate:\n");
        std::printf("    · business thread does proto+zlib+AES, then memcpy into the current cache (buffer-*.mmap)\n");
        std::printf("    · cache fills up -> swap; old cache goes onto the strand for: Seal -> msync -> ForEachFrame\n");
        std::printf("    · ForEachFrame copies each frame into the active shard (log-*.bin)\n");
        std::printf("    · cache is then ResetForReuse and returns to the pool — same file, reused as a ring\n");
        std::printf("    · so cache file size = mmap_buffer_bytes (1 MiB here); shard files keep growing\n");
        std::printf("\n");

        // 5. Sanity check: decode in-process and show the first 3 lines so you
        //    can see the plaintext round-trip works.
        const std::string priv_str(reinterpret_cast<const char*>(kp.priv.BytePtr()),
                                   kp.priv.SizeInBytes());
        const auto records = logger::audit::DecodeShardDir(demo_dir, priv_str);
        std::printf("[4/4] in-process decode round-trip: %zu records recovered.\n", records.size());
        std::printf("        sample (first 3 lines):\n");
        for (std::size_t i = 0; i < records.size() && i < 3; ++i) {
            std::printf("          [%s] %s\n",
                        LevelName(records[i].level()).c_str(),
                        records[i].message().c_str());
        }
        std::printf("\n");

        // 6. Tell the user exactly how to decrypt offline.
        std::printf("=============================================================\n");
        std::printf("How to decrypt the log files offline\n");
        std::printf("=============================================================\n");
        std::printf("\n");
        std::printf("All %d records are encrypted on disk inside the log-*.bin file(s)\n", kNumRecords);
        std::printf("listed above. To recover the plaintext, run the standalone CLI:\n");
        std::printf("\n");
        std::printf("  Plain text output:\n");
        std::printf("    D:/logger_high/build/audit/examples/Debug/log_decoder.exe ^\n");
        std::printf("      \"%s\" ^\n", demo_dir.string().c_str());
        std::printf("      \"%s\"\n", priv_path.string().c_str());
        std::printf("\n");
        std::printf("  JSON Lines output:\n");
        std::printf("    D:/logger_high/build/audit/examples/Debug/log_decoder.exe ^\n");
        std::printf("      \"%s\" ^\n", demo_dir.string().c_str());
        std::printf("      \"%s\" --json\n", priv_path.string().c_str());
        std::printf("\n");
        std::printf("The CLI does these steps internally:\n");
        std::printf("  1. read the 48-byte shard header  -> recover client_eph_pub (32B X25519)\n");
        std::printf("  2. X25519 ECDH(server_priv, client_eph_pub) -> 32B shared secret\n");
        std::printf("  3. HKDF-SHA256(shared, info=\"logger-high aes-256-gcm\") -> 32B AES key\n");
        std::printf("  4. for each frame: AES-256-GCM Decrypt -> zlib Decompress -> proto::LogRecord parse\n");
        std::printf("  5. print plaintext to stdout\n");
        std::printf("\n");
        std::printf("Note: in a real deployment server.priv NEVER leaves the audit machine;\n");
        std::printf("      only server.pub is shipped to the logging hosts. This demo writes\n");
        std::printf("      both into the same directory for convenience.\n");
        std::printf("\n");

        ctx.Shutdown();

        std::printf("Press Enter to close this window...\n");
        std::getchar();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "encrypted_logging_demo: %s\n", e.what());
        std::fprintf(stderr, "Press Enter to close this window...\n");
        std::getchar();
        return 1;
    }
}
