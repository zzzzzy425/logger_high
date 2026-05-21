#include "sinks/effective_sink.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "compress/zlib_compressor.h"
#include "crypt/aes_gcm_cipher.h"
#include "crypt/kdf.h"
#include "crypt/server_key_loader.h"
#include "crypt/x25519_agreement.h"
#include "utils/internal_log.h"
#include "utils/log_level.h"

namespace logger::sinks {

namespace {

void SecureZero(std::string& s) {
    if (s.empty()) {
        return;
    }
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); ++i) {
        p[i] = 0;
    }
}

}  // namespace

EffectiveSink::EffectiveSink(const EffectiveSinkConfig& cfg,
                             std::shared_ptr<context::Strand> strand,
                             std::unique_ptr<formatter::EffectiveFormatter> fmt,
                             std::unique_ptr<compress::ICompressor> compressor,
                             std::unique_ptr<crypt::ICipher> cipher,
                             std::string client_eph_pub)
    : fmt_(std::move(fmt)),
      compressor_(std::move(compressor)),
      cipher_(std::move(cipher)) {
    if (!fmt_ || !compressor_ || !cipher_) {
        throw std::runtime_error("EffectiveSink: null component injected");
    }
    if (!strand) {
        throw std::runtime_error("EffectiveSink: strand must not be null");
    }

    shard_writer_ = std::make_unique<ShardWriter>(
        cfg.log_dir, cfg.shard_size_bytes, std::move(client_eph_pub));

    evictor_ = std::make_unique<EvictionRunner>(
        cfg.log_dir, cfg.dir_size_cap, cfg.eviction_interval,
        [this] { return shard_writer_->CurrentShardPath(); });
    evictor_->Start();

    mmap::MmapStore::Config mcfg;
    mcfg.dir                   = cfg.log_dir;
    mcfg.buffer_bytes          = cfg.mmap_buffer_bytes;
    mcfg.min_buffers           = cfg.mmap_min_buffers;
    mcfg.max_buffers           = cfg.mmap_max_buffers;
    mcfg.back_pressure_timeout = cfg.back_pressure_timeout;

    mmap_store_ = std::make_unique<mmap::MmapStore>(
        mcfg, std::move(strand),
        [this](const std::byte* d, std::size_t n) { OnPersist_(d, n); });
}

EffectiveSink::~EffectiveSink() {
    try {
        if (mmap_store_) {
            mmap_store_->Flush();
        }
        if (shard_writer_) {
            shard_writer_->Flush();
        }
        if (evictor_) {
            evictor_->Stop();
        }
    } catch (...) {
    }
}

void EffectiveSink::Log(const LogMsg& msg) {
    std::lock_guard<std::mutex> lk(pipeline_mu_);
    buf_proto_.clear();
    buf_compressed_.clear();
    buf_encrypted_.clear();
    buf_frame_.clear();

    fmt_->Format(msg, buf_proto_);
    compressor_->Compress(buf_proto_, buf_compressed_);
    cipher_->Encrypt(buf_compressed_, buf_encrypted_);

    const std::uint32_t n = static_cast<std::uint32_t>(buf_encrypted_.size());
    const unsigned char len_le[4] = {
        static_cast<unsigned char>(n & 0xFF),
        static_cast<unsigned char>((n >> 8) & 0xFF),
        static_cast<unsigned char>((n >> 16) & 0xFF),
        static_cast<unsigned char>((n >> 24) & 0xFF),
    };
    buf_frame_.append(reinterpret_cast<const char*>(len_le), 4);
    buf_frame_.append(buf_encrypted_);

    const bool ok = mmap_store_->Append(
        reinterpret_cast<const std::byte*>(buf_frame_.data()), buf_frame_.size());
    if (!ok) {
        utils::InternalLog(LogLevel::error, "EffectiveSink",
                           "back-pressure timeout, dropped 1 record");
    }
}

void EffectiveSink::Flush() {
    if (mmap_store_) {
        mmap_store_->Flush();
    }
    if (shard_writer_) {
        shard_writer_->Flush();
    }
}

void EffectiveSink::OnPersist_(const std::byte* data, std::size_t len) {
    try {
        shard_writer_->WriteFrame(data, len);
    } catch (const std::exception& e) {
        utils::InternalLogF(LogLevel::error, "EffectiveSink",
                            "OnPersist write failed: {}", e.what());
    } catch (...) {
        utils::InternalLog(LogLevel::error, "EffectiveSink",
                           "OnPersist write failed: unknown");
    }
}

std::unique_ptr<EffectiveSink> EffectiveSink::Create(
    const EffectiveSinkConfig& cfg,
    std::shared_ptr<context::Strand> strand) {
    std::string server_pub = crypt::LoadServerPublicKey(cfg.server_pub_key_path);

    crypt::X25519KeyAgreement ka;
    std::string client_eph_pub;
    std::string shared;
    ka.DeriveSharedSecret(server_pub, client_eph_pub, shared);

    std::string aes_key;
    crypt::HkdfSha256(shared, /*salt=*/"", /*info=*/"logger-high aes-256-gcm",
                      32, aes_key);
    SecureZero(shared);

    auto cipher = std::make_unique<crypt::AesGcmCipher>(aes_key);
    SecureZero(aes_key);

    auto compressor = std::make_unique<compress::ZlibCompressor>(cfg.zlib_level);
    auto fmt        = std::make_unique<formatter::EffectiveFormatter>();

    return std::make_unique<EffectiveSink>(cfg, std::move(strand),
                                           std::move(fmt),
                                           std::move(compressor),
                                           std::move(cipher),
                                           std::move(client_eph_pub));
}

}  // namespace logger::sinks
