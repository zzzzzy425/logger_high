#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "mmap/mmap_region.h"

namespace logger::mmap {

// 一段 mmap region 上的"帧化追加缓冲"。
//
// 生命周期：write-phase → Seal() → drain-phase（ForEachFrame）→ ResetForReuse() → write-phase ...
//
// 写阶段：多生产者并发 Write 安全。每条 Write 在 buffer 内写入
//   [uint32_le len][payload bytes]。fetch_add(offset_) 越界后**不回滚**，
//   buffer 自动进入"写不下"状态，等调用方 Seal + swap。
//
// 密封后：Seal 阻塞等待所有 in-flight 写者完成，保证 drain 读到一个静止快照。
class MmapBuffer {
public:
    MmapBuffer(const std::filesystem::path& path, std::size_t capacity_bytes);

    MmapBuffer(const MmapBuffer&)            = delete;
    MmapBuffer& operator=(const MmapBuffer&) = delete;
    MmapBuffer(MmapBuffer&&)                 = delete;
    MmapBuffer& operator=(MmapBuffer&&)      = delete;

    // 多生产者并发安全。返回 false 表示剩余空间不足或 buffer 已 Seal。
    bool Write(const std::byte* data, std::size_t len);

    // 标记 buffer 不再接收新写者；阻塞等待 in-flight 写者退出。
    // 之后调用 ForEachFrame / ResetForReuse 才是安全的。
    void Seal();

    // 仅在 Seal 之后调用。顺序遍历所有已写入的完整帧。
    template <class Fn>
    void ForEachFrame(Fn&& fn) const {
        const auto* base = region_.data();
        const std::size_t cap = region_.size();
        const std::size_t end = std::min<std::size_t>(offset_.load(std::memory_order_acquire), cap);
        std::size_t pos = 0;
        while (pos + 4 <= end) {
            std::uint32_t len = 0;
            std::memcpy(&len, base + pos, 4);
            if (len == 0 || pos + 4 + len > end) {
                break;  // 未提交或越界 — drain 前 Seal 已等所有写者退出，理论不该发生
            }
            fn(base + pos + 4, static_cast<std::size_t>(len));
            pos += 4 + len;
        }
    }

    // 仅在 Seal 之后调用。把 buffer 重置为可写状态。
    void ResetForReuse();

    // 整段 msync。drain 时落盘前调。
    void Sync();

    std::size_t Used() const noexcept;
    std::size_t Capacity() const noexcept { return region_.size(); }
    const std::filesystem::path& Path() const noexcept { return region_.path(); }

private:
    MmapRegion          region_;
    std::atomic<std::size_t> offset_{0};
    std::atomic<std::size_t> writers_{0};
    std::atomic<bool>        sealed_{false};
};

}  // namespace logger::mmap
