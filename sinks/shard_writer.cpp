#include "sinks/shard_writer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fmt/format.h>

#include "utils/internal_log.h"
#include "utils/log_level.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace logger::sinks {

namespace {

void EncodeLEU32(std::uint32_t v, std::byte* out) {
    out[0] = static_cast<std::byte>(v & 0xFF);
    out[1] = static_cast<std::byte>((v >> 8) & 0xFF);
    out[2] = static_cast<std::byte>((v >> 16) & 0xFF);
    out[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}

}  // namespace

ShardWriter::ShardWriter(std::filesystem::path dir,
                         std::size_t shard_limit,
                         std::string client_eph_pub)
    : dir_(std::move(dir)),
      shard_limit_(shard_limit),
      client_eph_pub_(std::move(client_eph_pub)) {
    if (client_eph_pub_.size() != 32) {
        throw std::runtime_error("ShardWriter: client_eph_pub must be 32 bytes");
    }
    if (shard_limit_ <= kHeaderBytes) {
        throw std::runtime_error("ShardWriter: shard_limit must exceed header size");
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        throw std::runtime_error("ShardWriter: failed to create dir " +
                                 dir_.string() + ": " + ec.message());
    }
    std::lock_guard<std::mutex> lk(mu_);
    OpenNewShard_();
}

ShardWriter::~ShardWriter() {
    std::lock_guard<std::mutex> lk(mu_);
    CloseCurrent_();
}

void ShardWriter::WriteFrame(const std::byte* data, std::size_t len) {
    if (len == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (file_ == nullptr) {
        OpenNewShard_();
    }
    if (current_size_ > kHeaderBytes && current_size_ + len > shard_limit_) {
        CloseCurrent_();
        OpenNewShard_();
    }
    const std::size_t wrote = std::fwrite(data, 1, len, file_);
    if (wrote != len) {
        utils::InternalLogF(LogLevel::error, "ShardWriter",
                            "fwrite short ({} of {}) on {}",
                            wrote, len, current_path_.string());
        return;
    }
    current_size_ += len;
}

std::filesystem::path ShardWriter::CurrentShardPath() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_path_;
}

void ShardWriter::Flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (file_ == nullptr) {
        return;
    }
    std::fflush(file_);
#ifdef _WIN32
    const int fd = _fileno(file_);
    if (fd >= 0) {
        _commit(fd);
    }
#else
    const int fd = fileno(file_);
    if (fd >= 0) {
        ::fsync(fd);
    }
#endif
}

void ShardWriter::OpenNewShard_() {
    const auto name = MakeShardName_();
    current_path_   = dir_ / name;
#ifdef _WIN32
    if (fopen_s(&file_, current_path_.string().c_str(), "wb") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(current_path_.string().c_str(), "wb");
#endif
    if (file_ == nullptr) {
        throw std::runtime_error("ShardWriter: failed to open shard file " +
                                 current_path_.string());
    }
    current_size_ = 0;
    WriteHeader_();
}

void ShardWriter::CloseCurrent_() {
    if (file_ == nullptr) {
        return;
    }
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
}

void ShardWriter::WriteHeader_() {
    std::byte hdr[kHeaderBytes] = {};
    const char kMagic[8] = {'L', 'G', 'R', 'H', '\x01', '\x00', '\x00', '\x00'};
    std::memcpy(hdr, kMagic, 8);
    EncodeLEU32(kAlgoIdAesGcmX25519HkdfSha256, hdr + 8);
    std::memcpy(hdr + 12, client_eph_pub_.data(), 32);
    // hdr[44..47] are already zero (reserved)
    const std::size_t wrote = std::fwrite(hdr, 1, kHeaderBytes, file_);
    if (wrote != kHeaderBytes) {
        throw std::runtime_error("ShardWriter: short header write on " +
                                 current_path_.string());
    }
    current_size_ = kHeaderBytes;
}

std::string ShardWriter::MakeShardName_() {
    using clock = std::chrono::system_clock;
    const auto now    = clock::now();
    const auto epoch  = now.time_since_epoch();
    const auto sec    = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count() % 1000;
    const auto tt     = static_cast<std::time_t>(sec);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    const std::uint32_t s = seq_++;
    return fmt::format("log-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{:04}.bin",
                       tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                       static_cast<int>(millis), s);
}

}  // namespace logger::sinks
