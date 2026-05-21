#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "compress/compressor.h"
#include "context/strand.h"
#include "crypt/cipher.h"
#include "formatter/effective_formatter.h"
#include "mmap/mmap_store.h"
#include "sinks/effective_sink_config.h"
#include "sinks/eviction_runner.h"
#include "sinks/log_sink.h"
#include "sinks/shard_writer.h"

namespace logger::sinks {

// Production sink: per-record pipeline of EffectiveFormatter -> Compressor ->
// AEAD cipher -> MmapStore -> ShardWriter. Initialization performs a one-shot
// X25519 ECDH against a server static public key, derives the AES-256 key
// via HKDF-SHA256, and embeds the client ephemeral public key in every
// shard file's header so an offline auditor with the server private key can
// recover the AES key.
class EffectiveSink final : public LogSink {
public:
    EffectiveSink(const EffectiveSinkConfig& cfg,
                  std::shared_ptr<context::Strand> strand,
                  std::unique_ptr<formatter::EffectiveFormatter> fmt,
                  std::unique_ptr<compress::ICompressor> compressor,
                  std::unique_ptr<crypt::ICipher> cipher,
                  std::string client_eph_pub);
    ~EffectiveSink() override;

    void Log(const LogMsg& msg) override;
    void Flush() override;

    // Factory: loads the server public key from cfg.server_pub_key_path,
    // performs X25519 + HKDF-SHA256, and constructs the full pipeline.
    static std::unique_ptr<EffectiveSink> Create(
        const EffectiveSinkConfig& cfg,
        std::shared_ptr<context::Strand> strand);

private:
    void OnPersist_(const std::byte* data, std::size_t len);

    std::mutex                                          pipeline_mu_;
    std::string                                         buf_proto_;
    std::string                                         buf_compressed_;
    std::string                                         buf_encrypted_;
    std::string                                         buf_frame_;

    std::unique_ptr<formatter::EffectiveFormatter>      fmt_;
    std::unique_ptr<compress::ICompressor>              compressor_;
    std::unique_ptr<crypt::ICipher>                     cipher_;

    std::unique_ptr<ShardWriter>                        shard_writer_;
    std::unique_ptr<EvictionRunner>                     evictor_;
    std::unique_ptr<mmap::MmapStore>                    mmap_store_;
};

}  // namespace logger::sinks
