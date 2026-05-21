// EffectiveSink end-to-end smoke test.
//
// Six cases (see plan humming-growing-leaf.md):
//   1. end-to-end encrypt/decrypt round trip (1000 records)
//   2. auto sharding produces multiple shard files
//   3. eviction caps directory size while active sink keeps writing
//   4. 8 producer threads x 5000 records: every record recoverable
//   5. back-pressure timeout path does not throw or deadlock
//   6. shard file header matches the v1 layout

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cryptopp/osrng.h>
#include <cryptopp/secblock.h>
#include <cryptopp/xed25519.h>

#include "compress/zlib_compressor.h"
#include "context/context.h"
#include "context/strand.h"
#include "crypt/aes_gcm_cipher.h"
#include "crypt/kdf.h"
#include "handle/log_msg.h"
#include "proto/log_record.pb.h"
#include "sinks/effective_sink.h"
#include "sinks/effective_sink_config.h"
#include "sinks/shard_writer.h"
#include "utils/log_level.h"
#include "utils/source_location.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);\
        std::exit(1);                                                        \
    }                                                                        \
} while (0)

namespace {

fs::path MakeTempDir(const char* tag) {
    auto base = fs::temp_directory_path() / "logger_high_eff_smoke";
    fs::create_directories(base);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s-%lld", tag,
                  static_cast<long long>(
                      std::chrono::steady_clock::now().time_since_epoch().count()));
    auto p = base / buf;
    fs::create_directories(p);
    return p;
}

void RemoveAll(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

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

fs::path WriteServerPubFile(const AuditKeypair& kp, const fs::path& dir) {
    const auto p = dir / "server.pub";
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(kp.pub.BytePtr()),
              static_cast<std::streamsize>(kp.pub.SizeInBytes()));
    out.close();
    return p;
}

bool IsShardFile(const std::string& name) {
    return name.size() > 8 &&
           name.compare(0, 4, "log-") == 0 &&
           name.compare(name.size() - 4, 4, ".bin") == 0;
}

std::vector<fs::path> ListShards(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& de : fs::directory_iterator(dir)) {
        if (!de.is_regular_file()) {
            continue;
        }
        if (IsShardFile(de.path().filename().string())) {
            out.push_back(de.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uintmax_t DirShardTotalSize(const fs::path& dir) {
    std::uintmax_t total = 0;
    for (const auto& p : ListShards(dir)) {
        std::error_code ec;
        total += fs::file_size(p, ec);
    }
    return total;
}

std::uint32_t DecodeLEU32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0])) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

struct ShardHeader {
    std::string client_eph_pub;  // 32B
    std::uint32_t algo_id;
};

ShardHeader ReadShardHeader(std::ifstream& in) {
    char hdr[logger::sinks::ShardWriter::kHeaderBytes];
    in.read(hdr, sizeof(hdr));
    CHECK(static_cast<std::size_t>(in.gcount()) == sizeof(hdr),
          "short header read");
    const unsigned char kMagic[8] = {'L', 'G', 'R', 'H', 0x01, 0x00, 0x00, 0x00};
    CHECK(std::memcmp(hdr, kMagic, 8) == 0, "shard magic mismatch");
    ShardHeader h;
    h.algo_id = DecodeLEU32(reinterpret_cast<const unsigned char*>(hdr + 8));
    h.client_eph_pub.assign(hdr + 12, 32);
    return h;
}

std::vector<logger::proto::LogRecord> DecodeShard(const fs::path& path,
                                                  const AuditKeypair& kp) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good(), "cannot open shard for read");
    ShardHeader hdr = ReadShardHeader(in);
    CHECK(hdr.algo_id == 1u, "shard algo_id mismatch");

    // ECDH against the client's ephemeral public key recovered from the header.
    CryptoPP::x25519       dh;
    CryptoPP::SecByteBlock shared(32);
    const bool ok = dh.Agree(
        shared.BytePtr(), kp.priv.BytePtr(),
        reinterpret_cast<const CryptoPP::byte*>(hdr.client_eph_pub.data()), true);
    CHECK(ok, "audit-side Agree failed");

    const std::string shared_str(reinterpret_cast<const char*>(shared.BytePtr()), 32);
    std::string aes_key;
    logger::crypt::HkdfSha256(shared_str, /*salt=*/"",
                              "logger-high aes-256-gcm", 32, aes_key);
    logger::crypt::AesGcmCipher cipher(aes_key);
    logger::compress::ZlibCompressor decompressor;

    std::vector<logger::proto::LogRecord> records;
    while (true) {
        unsigned char lenbuf[4];
        in.read(reinterpret_cast<char*>(lenbuf), 4);
        if (in.gcount() == 0) {
            break;
        }
        CHECK(in.gcount() == 4, "truncated frame length");
        const std::uint32_t flen = DecodeLEU32(lenbuf);
        CHECK(flen >= 28, "frame too small to contain nonce+tag");
        std::string encrypted(flen, '\0');
        in.read(encrypted.data(), static_cast<std::streamsize>(flen));
        CHECK(static_cast<std::uint32_t>(in.gcount()) == flen, "short frame read");

        std::string compressed;
        cipher.Decrypt(encrypted, compressed);
        std::string proto_bytes;
        decompressor.Decompress(compressed, proto_bytes);

        logger::proto::LogRecord rec;
        CHECK(rec.ParseFromString(proto_bytes), "protobuf parse failed");
        records.push_back(std::move(rec));
    }
    return records;
}

std::vector<logger::proto::LogRecord> DecodeAllShards(const fs::path& dir,
                                                      const AuditKeypair& kp) {
    std::vector<logger::proto::LogRecord> out;
    for (const auto& p : ListShards(dir)) {
        auto recs = DecodeShard(p, kp);
        for (auto& r : recs) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

logger::LogMsg MakeMsg(const std::string& body) {
    logger::LogMsg m;
    m.level       = logger::LogLevel::info;
    m.time        = std::chrono::system_clock::now();
    m.logger_name = "smoke";
    m.message     = body;
    m.loc         = logger::SourceLocation{};
    return m;
}

logger::sinks::EffectiveSinkConfig BaseConfig(const fs::path& dir,
                                              const fs::path& pub_path) {
    logger::sinks::EffectiveSinkConfig cfg;
    cfg.log_dir              = dir;
    cfg.server_pub_key_path  = pub_path;
    cfg.shard_size_bytes     = 64ull * 1024 * 1024;
    cfg.dir_size_cap         = 2ull * 1024 * 1024 * 1024;
    cfg.eviction_interval    = 30s;
    cfg.mmap_buffer_bytes    = 4ull * 1024 * 1024;
    cfg.mmap_min_buffers     = 2;
    cfg.mmap_max_buffers     = 8;
    cfg.back_pressure_timeout = 500ms;
    cfg.zlib_level           = 6;
    return cfg;
}

// ---------------------------------------------------------------------------

void Case1_EndToEnd() {
    auto dir   = MakeTempDir("c1_e2e");
    auto kp    = MakeAuditKeypair();
    auto pub_p = WriteServerPubFile(kp, dir);

    auto& ctx    = logger::context::Context::Instance();
    auto  strand = ctx.MakeStrand();

    constexpr int N = 1000;
    {
        auto cfg = BaseConfig(dir, pub_p);
        auto sink = logger::sinks::EffectiveSink::Create(cfg, strand);
        for (int i = 0; i < N; ++i) {
            std::ostringstream oss;
            oss << "case1 record " << i;
            const std::string body = oss.str();
            sink->Log(MakeMsg(body));
        }
        sink->Flush();
    }

    auto recs = DecodeAllShards(dir, kp);
    CHECK(static_cast<int>(recs.size()) == N, "case1 record count mismatch");
    for (int i = 0; i < N; ++i) {
        std::ostringstream oss;
        oss << "case1 record " << i;
        CHECK(recs[i].message() == oss.str(), "case1 message mismatch");
        CHECK(recs[i].logger_name() == "smoke", "case1 logger_name mismatch");
        CHECK(recs[i].level() == static_cast<std::uint32_t>(logger::LogLevel::info),
              "case1 level mismatch");
    }

    RemoveAll(dir);
    std::puts("[ok] Case1_EndToEnd");
}

void Case2_AutoShard() {
    auto dir   = MakeTempDir("c2_shard");
    auto kp    = MakeAuditKeypair();
    auto pub_p = WriteServerPubFile(kp, dir);

    auto& ctx    = logger::context::Context::Instance();
    auto  strand = ctx.MakeStrand();

    constexpr int N = 10000;
    {
        auto cfg = BaseConfig(dir, pub_p);
        cfg.shard_size_bytes = 256ull * 1024;  // 256 KiB
        cfg.mmap_buffer_bytes = 64ull * 1024;
        auto sink = logger::sinks::EffectiveSink::Create(cfg, strand);
        for (int i = 0; i < N; ++i) {
            std::ostringstream oss;
            oss << "c2 " << i;
            sink->Log(MakeMsg(oss.str()));
        }
        sink->Flush();
    }

    auto shards = ListShards(dir);
    CHECK(shards.size() >= 2, "case2 expected multiple shards");
    auto recs = DecodeAllShards(dir, kp);
    CHECK(static_cast<int>(recs.size()) == N, "case2 total frame count mismatch");

    RemoveAll(dir);
    std::printf("[ok] Case2_AutoShard (shards=%zu)\n", shards.size());
}

void Case3_Eviction() {
    auto dir   = MakeTempDir("c3_evict");
    auto kp    = MakeAuditKeypair();
    auto pub_p = WriteServerPubFile(kp, dir);

    auto& ctx    = logger::context::Context::Instance();
    auto  strand = ctx.MakeStrand();

    auto cfg = BaseConfig(dir, pub_p);
    cfg.shard_size_bytes  = 64ull * 1024;
    cfg.dir_size_cap      = 256ull * 1024;
    cfg.eviction_interval = 1s;
    cfg.mmap_buffer_bytes = 32ull * 1024;

    // The cap is 256 KiB; on top of that one active shard up to 64 KiB may
    // exist. Allow generous slack for in-flight mmap buffers.
    const std::uintmax_t slack_limit = cfg.dir_size_cap + cfg.shard_size_bytes * 2;

    {
        auto sink = logger::sinks::EffectiveSink::Create(cfg, strand);
        // Write enough bytes to overflow the cap several times.
        constexpr int N = 8000;
        const std::string body(64, 'x');
        for (int i = 0; i < N; ++i) {
            sink->Log(MakeMsg(body));
            if ((i & 0xFF) == 0) {
                std::this_thread::sleep_for(2ms);
            }
        }
        sink->Flush();
        std::this_thread::sleep_for(3500ms);  // let eviction sweep a few times

        const auto total = DirShardTotalSize(dir);
        std::fprintf(stderr, "  case3: dir_total=%llu cap=%llu slack_limit=%llu\n",
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(cfg.dir_size_cap),
                     static_cast<unsigned long long>(slack_limit));
        CHECK(total <= slack_limit, "case3: directory grew past slack");
    }

    RemoveAll(dir);
    std::puts("[ok] Case3_Eviction");
}

void Case4_MultiThread() {
    auto dir   = MakeTempDir("c4_mt");
    auto kp    = MakeAuditKeypair();
    auto pub_p = WriteServerPubFile(kp, dir);

    auto& ctx    = logger::context::Context::Instance();
    auto  strand = ctx.MakeStrand();

    constexpr int kThreads   = 8;
    constexpr int kPerThread = 5000;

    {
        auto cfg = BaseConfig(dir, pub_p);
        cfg.shard_size_bytes = 512ull * 1024;
        cfg.mmap_buffer_bytes = 64ull * 1024;
        cfg.mmap_max_buffers  = 16;
        cfg.back_pressure_timeout = 2s;
        auto sink = logger::sinks::EffectiveSink::Create(cfg, strand);

        std::vector<std::thread> ths;
        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    std::ostringstream oss;
                    oss << "t" << t << ":s" << i;
                    sink->Log(MakeMsg(oss.str()));
                }
            });
        }
        for (auto& th : ths) th.join();
        sink->Flush();
    }

    auto recs = DecodeAllShards(dir, kp);
    CHECK(static_cast<int>(recs.size()) == kThreads * kPerThread,
          "case4 record count mismatch");
    std::set<std::string> seen;
    for (const auto& r : recs) {
        seen.insert(r.message());
    }
    CHECK(static_cast<int>(seen.size()) == kThreads * kPerThread,
          "case4 seen unique count mismatch");
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            std::ostringstream oss;
            oss << "t" << t << ":s" << i;
            CHECK(seen.count(oss.str()) == 1, "case4 missing entry");
        }
    }

    RemoveAll(dir);
    std::puts("[ok] Case4_MultiThread");
}

void Case5_BackpressureSurvives() {
    auto dir   = MakeTempDir("c5_bp");
    auto kp    = MakeAuditKeypair();
    auto pub_p = WriteServerPubFile(kp, dir);

    auto& ctx    = logger::context::Context::Instance();
    auto  strand = ctx.MakeStrand();

    auto cfg = BaseConfig(dir, pub_p);
    cfg.mmap_buffer_bytes      = 16ull * 1024;
    cfg.mmap_min_buffers       = 2;
    cfg.mmap_max_buffers       = 2;
    cfg.back_pressure_timeout  = 10ms;
    cfg.shard_size_bytes       = 256ull * 1024;

    {
        auto sink = logger::sinks::EffectiveSink::Create(cfg, strand);
        const std::string body(512, 'B');
        std::vector<std::thread> ths;
        std::atomic<bool> died{false};
        for (int t = 0; t < 8; ++t) {
            ths.emplace_back([&]() {
                try {
                    for (int i = 0; i < 2000; ++i) {
                        sink->Log(MakeMsg(body));
                    }
                } catch (...) {
                    died.store(true);
                }
            });
        }
        for (auto& th : ths) th.join();
        CHECK(!died.load(), "case5: Log() threw under back-pressure");
        sink->Flush();
    }

    RemoveAll(dir);
    std::puts("[ok] Case5_BackpressureSurvives");
}

void Case6_HeaderFormat() {
    auto dir   = MakeTempDir("c6_hdr");
    auto kp    = MakeAuditKeypair();
    auto pub_p = WriteServerPubFile(kp, dir);

    auto& ctx    = logger::context::Context::Instance();
    auto  strand = ctx.MakeStrand();

    {
        auto cfg = BaseConfig(dir, pub_p);
        cfg.shard_size_bytes = 64ull * 1024;
        cfg.mmap_buffer_bytes = 16ull * 1024;
        auto sink = logger::sinks::EffectiveSink::Create(cfg, strand);
        for (int i = 0; i < 100; ++i) {
            sink->Log(MakeMsg("header check"));
        }
        sink->Flush();
    }

    auto shards = ListShards(dir);
    CHECK(!shards.empty(), "case6 no shards produced");

    std::ifstream in(shards.front(), std::ios::binary);
    CHECK(in.good(), "case6 cannot open shard");
    char hdr[logger::sinks::ShardWriter::kHeaderBytes];
    in.read(hdr, sizeof(hdr));
    CHECK(static_cast<std::size_t>(in.gcount()) == sizeof(hdr),
          "case6 short header read");
    const unsigned char kMagic[8] = {'L', 'G', 'R', 'H', 0x01, 0x00, 0x00, 0x00};
    CHECK(std::memcmp(hdr, kMagic, 8) == 0, "case6 magic mismatch");
    const std::uint32_t algo_id =
        DecodeLEU32(reinterpret_cast<const unsigned char*>(hdr + 8));
    CHECK(algo_id == 1u, "case6 algo_id mismatch");
    // pub key in header must be a valid 32B X25519 pub (non-zero) — check it
    // is not all zeros.
    bool nonzero = false;
    for (int i = 12; i < 44; ++i) {
        if (hdr[i] != 0) {
            nonzero = true;
            break;
        }
    }
    CHECK(nonzero, "case6 client_eph_pub all zero");
    // reserved 4 bytes must be zero
    for (int i = 44; i < 48; ++i) {
        CHECK(hdr[i] == 0, "case6 reserved byte non-zero");
    }

    RemoveAll(dir);
    std::puts("[ok] Case6_HeaderFormat");
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    Case1_EndToEnd();
    Case2_AutoShard();
    Case3_Eviction();
    Case4_MultiThread();
    Case5_BackpressureSurvives();
    Case6_HeaderFormat();
    logger::context::Context::Instance().Shutdown();
    google::protobuf::ShutdownProtobufLibrary();
    std::puts("effective_sink_smoke: all cases passed");
    return 0;
}
