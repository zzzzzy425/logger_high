#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>

namespace logger::sinks {

struct EffectiveSinkConfig {
    // Required.
    std::filesystem::path log_dir;
    std::filesystem::path server_pub_key_path;

    // Sharding & eviction.
    std::size_t              shard_size_bytes   = 64ull * 1024 * 1024;  // 64 MiB
    std::size_t              dir_size_cap       = 2ull * 1024 * 1024 * 1024;  // 2 GiB
    std::chrono::seconds     eviction_interval{30};

    // Forwarded to MmapStore::Config.
    std::size_t                mmap_buffer_bytes = 4ull * 1024 * 1024;
    std::size_t                mmap_min_buffers  = 2;
    std::size_t                mmap_max_buffers  = 8;
    std::chrono::milliseconds  back_pressure_timeout{200};

    // zlib level (1 fastest .. 9 best). 6 is zlib's default.
    int zlib_level = 6;
};

}  // namespace logger::sinks
