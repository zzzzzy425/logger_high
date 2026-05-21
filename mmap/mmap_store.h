#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

#include "context/strand.h"
#include "mmap/mmap_buffer.h"

namespace logger::mmap {

// MmapStore：主从双缓冲 + 动态扩容池 + Strand 串行 drain。
//
// 生产者：Append(bytes, len) 把字节追加进当前 master buffer；写满自动 swap，
// 把旧 buffer 投给 strand 异步 drain。drain 回调用户传入的 on_persist，
// 由它决定最终怎么落盘（拼到日志文件、远传等）。
//
// 不持有 strand 的所有权（shared_ptr 由调用方提供，与 logger::context::Context 一致）。
class MmapStore {
public:
    using PersistFn = std::function<void(const std::byte* data, std::size_t len)>;

    struct Config {
        std::filesystem::path dir;                                  // buffer 文件目录
        std::size_t buffer_bytes  = 4 * 1024 * 1024;                // 单 buffer 4MB
        std::size_t min_buffers   = 2;
        std::size_t max_buffers   = 8;
        std::chrono::milliseconds back_pressure_timeout{200};
    };

    MmapStore(Config cfg,
              std::shared_ptr<context::Strand> strand,
              PersistFn on_persist);
    ~MmapStore();

    MmapStore(const MmapStore&)            = delete;
    MmapStore& operator=(const MmapStore&) = delete;
    MmapStore(MmapStore&&)                 = delete;
    MmapStore& operator=(MmapStore&&)      = delete;

    // 把字节追加进 store。线程安全。
    // 返回 false 仅当池满且 back_pressure_timeout 内未拿到空闲 buffer。
    bool Append(const std::byte* data, std::size_t len);

    // 阻塞直到当前已 Append 字节全部经过 on_persist。
    void Flush();

    // 已 drain 字节累计（不包含还在当前 master 里没 swap 的）。仅供观测/单测。
    std::size_t PersistedBytes() const noexcept;

    // 当前 pool 中 buffer 个数。仅供观测/单测。
    std::size_t PoolSize() const;

private:
    // 尝试取得一个空闲 buffer。可能从 free_ 取、扩容创建、或阻塞等待。
    // 调用前必须持有 mtx_。返回 nullptr 表示超时或已停止。
    // wait_forever=true 时忽略 cfg_.back_pressure_timeout，给 Flush 用。
    MmapBuffer* AcquireFreeBufferLocked_(std::unique_lock<std::mutex>& lk,
                                         bool wait_forever);

    // 切换 master 到一个新 buffer，把旧 buffer 投给 strand drain。
    // 调用前必须持有 mtx_。成功返回新 master 指针；失败返回 nullptr。
    MmapBuffer* SwapMasterLocked_(std::unique_lock<std::mutex>& lk,
                                  MmapBuffer* expected,
                                  bool wait_forever);

    // 在 strand 中执行：seal → sync → ForEachFrame(on_persist) → reset → 归还 free_。
    void DrainOne_(MmapBuffer* buf);

    std::filesystem::path BuildBufferPath_(std::uint64_t seq) const;

    Config                                    cfg_;
    std::shared_ptr<context::Strand>          strand_;
    PersistFn                                 on_persist_;

    mutable std::mutex                        mtx_;
    std::condition_variable                   free_cv_;
    std::condition_variable                   flush_cv_;
    std::deque<std::unique_ptr<MmapBuffer>>   pool_;
    std::queue<MmapBuffer*>                   free_;
    std::atomic<MmapBuffer*>                  master_{nullptr};
    std::atomic<std::size_t>                  in_flight_{0};
    std::atomic<std::size_t>                  persisted_bytes_{0};
    std::uint64_t                             next_seq_{0};
    bool                                      stopped_{false};
};

}  // namespace logger::mmap
