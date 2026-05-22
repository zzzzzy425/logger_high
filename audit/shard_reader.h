#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

#include "proto/log_record.pb.h"

// Audit-side decoder for shard files produced by logger::sinks::EffectiveSink /
// logger::sinks::ShardWriter. Given a server X25519 static private key, it
// recovers the per-shard AES-256 key from the shard header (which embeds the
// client's ephemeral public key), then decrypts and decompresses every frame
// into a logger::proto::LogRecord.
//
// All routines throw std::runtime_error on header/frame corruption, auth-tag
// mismatch, decompress failure, or proto parse failure.
namespace logger::audit {

struct ShardHeader {
    std::uint32_t algo_id        = 0;
    std::string   client_eph_pub;   // exactly 32 bytes for algo_id == 1
};

// Read and validate the 48B shard v1 header from the current position of `in`.
// On success `in` is positioned at the first frame.
ShardHeader ReadShardHeader(std::istream& in);

// Decode every frame in one shard file. server_priv must be exactly 32 bytes
// (X25519 raw private scalar). The vector preserves frame order in the file.
std::vector<proto::LogRecord> DecodeShardFile(const std::filesystem::path& shard,
                                              std::string_view             server_priv);

// Decode every `log-*.bin` in `dir`, sorted lexicographically (which equals
// chronological order under the project's filename convention).
std::vector<proto::LogRecord> DecodeShardDir(const std::filesystem::path& dir,
                                             std::string_view             server_priv);

}  // namespace logger::audit
