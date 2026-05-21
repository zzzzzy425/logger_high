#include "mmap/mmap_store.h"

#include <cstdio>
#include <exception>
#include <system_error>

namespace logger::mmap {

MmapStore::MmapStore(Config cfg,
                     std::shared_ptr<context::Strand> strand,
                     PersistFn on_persist)
    : cfg_(std::move(cfg)),
      strand_(std::move(strand)),
      on_persist_(std::move(on_persist)) {
    if (!strand_) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "MmapStore: strand is null");
    }
    if (!on_persist_) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "MmapStore: on_persist is null");
    }
    if (cfg_.min_buffers == 0) cfg_.min_buffers = 2;
    if (cfg_.max_buffers < cfg_.min_buffers) cfg_.max_buffers = cfg_.min_buffers;
    if (cfg_.buffer_bytes == 0) cfg_.buffer_bytes = 4 * 1024 * 1024;

    if (!cfg_.dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.dir, ec);
    }

    // 预创建 min_buffers 个 buffer：第 0 个作为 master，其余进 free_。
    for (std::size_t i = 0; i < cfg_.min_buffers; ++i) {
        pool_.emplace_back(std::make_unique<MmapBuffer>(
            BuildBufferPath_(next_seq_++), cfg_.buffer_bytes));
        if (i == 0) {
            master_.store(pool_.back().get(), std::memory_order_release);
        } else {
            free_.push(pool_.back().get());
        }
    }
}

MmapStore::~MmapStore() {
    try {
        Flush();
    } catch (...) {
        // never propagate from destructor
    }
    std::unique_lock<std::mutex> lk(mtx_);
    stopped_ = true;
    free_cv_.notify_all();
    flush_cv_.notify_all();
    // pool_ 析构会 unmap/close 所有 buffer。
}

std::filesystem::path MmapStore::BuildBufferPath_(std::uint64_t seq) const {
    char name[64];
    std::snprintf(name, sizeof(name), "buffer-%06llu.mmap",
                  static_cast<unsigned long long>(seq));
    if (cfg_.dir.empty()) {
        return std::filesystem::path(name);
    }
    return cfg_.dir / name;
}

bool MmapStore::Append(const std::byte* data, std::size_t len) {
    if (data == nullptr || len == 0) return false;

    for (;;) {
        MmapBuffer* cur = master_.load(std::memory_order_acquire);
        if (cur == nullptr) return false;
        if (cur->Write(data, len)) return true;

        // Write 失败 — 走 swap 路径。
        std::unique_lock<std::mutex> lk(mtx_);
        if (stopped_) return false;

        // 别的线程可能已经 swap 了。复查当前 master 能否容纳。
        MmapBuffer* now = master_.load(std::memory_order_acquire);
        if (now != cur) {
            // 已经 swap — 让外层循环到新 master 上重试。
            continue;
        }

        MmapBuffer* next = SwapMasterLocked_(lk, cur, /*wait_forever=*/false);
        if (next == nullptr) {
            // 满压超时。
            return false;
        }
        // 解锁后外层循环到新 master 重试 Write。
    }
}

MmapBuffer* MmapStore::SwapMasterLocked_(std::unique_lock<std::mutex>& lk,
                                         MmapBuffer* expected,
                                         bool wait_forever) {
    (void)expected;
    MmapBuffer* next = AcquireFreeBufferLocked_(lk, wait_forever);
    if (next == nullptr) return nullptr;

    MmapBuffer* old = master_.exchange(next, std::memory_order_acq_rel);
    in_flight_.fetch_add(1, std::memory_order_release);

    auto self_strand = strand_;
    self_strand->PostDetached([this, old]() { DrainOne_(old); });
    return next;
}

MmapBuffer* MmapStore::AcquireFreeBufferLocked_(std::unique_lock<std::mutex>& lk,
                                                bool wait_forever) {
    if (!free_.empty()) {
        MmapBuffer* b = free_.front();
        free_.pop();
        return b;
    }
    if (pool_.size() < cfg_.max_buffers) {
        try {
            pool_.emplace_back(std::make_unique<MmapBuffer>(
                BuildBufferPath_(next_seq_++), cfg_.buffer_bytes));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[logger::mmap] buffer create failed: %s\n", e.what());
            return nullptr;
        }
        return pool_.back().get();
    }

    // 池满 — 等空闲。
    if (wait_forever) {
        free_cv_.wait(lk, [&]() { return stopped_ || !free_.empty(); });
    } else {
        const bool got = free_cv_.wait_for(lk, cfg_.back_pressure_timeout, [&]() {
            return stopped_ || !free_.empty();
        });
        if (!got) return nullptr;
    }
    if (stopped_ || free_.empty()) return nullptr;
    MmapBuffer* b = free_.front();
    free_.pop();
    return b;
}

void MmapStore::DrainOne_(MmapBuffer* buf) {
    if (buf == nullptr) return;
    // 1) Seal：阻塞等待 in-flight 写者退出。
    buf->Seal();
    // 2) Sync：把脏页推到内核 / 磁盘。
    try {
        buf->Sync();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[logger::mmap] sync failed: %s\n", e.what());
    }
    // 3) ForEachFrame：把每一帧交给 on_persist。
    buf->ForEachFrame([this](const std::byte* p, std::size_t n) {
        try {
            on_persist_(p, n);
            persisted_bytes_.fetch_add(n, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[logger::mmap] on_persist threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[logger::mmap] on_persist threw unknown exception\n");
        }
    });
    // 4) Reset + 归还 free_。
    buf->ResetForReuse();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        free_.push(buf);
    }
    free_cv_.notify_one();

    const std::size_t left = in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        flush_cv_.notify_all();
    }
}

void MmapStore::Flush() {
    // 1) 把当前 master 也 swap 出来，进入 drain 队列。
    //    Flush 不能像 Append 那样吃 back-pressure 超时——必须等到拿到 buffer。
    {
        std::unique_lock<std::mutex> lk(mtx_);
        if (stopped_) return;
        MmapBuffer* cur = master_.load(std::memory_order_acquire);
        if (cur && cur->Used() > 0) {
            SwapMasterLocked_(lk, cur, /*wait_forever=*/true);
        }
    }
    // 2) 等 in_flight_ 归零。
    std::unique_lock<std::mutex> lk(mtx_);
    flush_cv_.wait(lk, [this]() {
        return in_flight_.load(std::memory_order_acquire) == 0 || stopped_;
    });
}

std::size_t MmapStore::PersistedBytes() const noexcept {
    return persisted_bytes_.load(std::memory_order_relaxed);
}

std::size_t MmapStore::PoolSize() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pool_.size();
}

}  // namespace logger::mmap
