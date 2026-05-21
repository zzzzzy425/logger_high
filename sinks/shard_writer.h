#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace logger::sinks {

// Owns the currently-active shard file. On overflow it rotates: close the old
// file, create a new one whose name encodes the open time so directory-listing
// in lexicographic order yields chronological order, and write a 48-byte v1
// header at the start of every shard. Header layout:
//   [8B magic = "LGRH\1\0\0\0"]
//   [4B algo_id  little-endian = 1 (AES-256-GCM+X25519+HKDF-SHA256)]
//   [32B client_eph_pub]
//   [4B reserved = 0]
// Frames written via WriteFrame() are appended verbatim after the header.
class ShardWriter {
public:
    static constexpr std::size_t kHeaderBytes = 48;
    static constexpr std::uint32_t kAlgoIdAesGcmX25519HkdfSha256 = 1;

    // client_eph_pub must be exactly 32 bytes (the X25519 ephemeral public key
    // that the auditor needs in order to reconstruct the AES key offline).
    ShardWriter(std::filesystem::path dir,
                std::size_t shard_limit,
                std::string client_eph_pub);
    ~ShardWriter();

    ShardWriter(const ShardWriter&)            = delete;
    ShardWriter& operator=(const ShardWriter&) = delete;
    ShardWriter(ShardWriter&&)                 = delete;
    ShardWriter& operator=(ShardWriter&&)      = delete;

    // Append one frame; rotates when the projected file size would exceed the
    // shard limit. Thread-safe; on persist callback fires on a single strand
    // but the EvictionRunner reads CurrentShardPath() concurrently, so the
    // mutex still matters.
    void WriteFrame(const std::byte* data, std::size_t len);

    std::filesystem::path CurrentShardPath() const;

    // Flush the current shard to disk (fflush; best-effort fsync on Windows).
    void Flush();

private:
    void OpenNewShard_();   // mutex must be held
    void CloseCurrent_();   // mutex must be held
    void WriteHeader_();    // mutex must be held, file_ must be open
    std::string MakeShardName_();  // mutex must be held (uses seq_)

    const std::filesystem::path     dir_;
    const std::size_t               shard_limit_;
    const std::string               client_eph_pub_;  // 32B

    mutable std::mutex              mu_;
    std::FILE*                      file_{nullptr};
    std::size_t                     current_size_{0};
    std::filesystem::path           current_path_;
    std::uint32_t                   seq_{0};         // disambiguate same-ms names
};

}  // namespace logger::sinks
