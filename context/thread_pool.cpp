#include "context/thread_pool.h"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace logger::context {

ThreadPool::Config ThreadPool::Config::Default() noexcept {
    Config c;
    const auto hw = std::thread::hardware_concurrency();
    const size_t base = hw == 0 ? 4 : static_cast<size_t>(hw);
    c.min_threads = std::max<size_t>(1, base / 4);
    c.max_threads = std::max<size_t>(c.min_threads + 1, base);
    return c;
}

ThreadPool::ThreadPool(Config cfg) : cfg_(cfg) {
    if (cfg_.min_threads == 0) cfg_.min_threads = 1;
    if (cfg_.max_threads < cfg_.min_threads) cfg_.max_threads = cfg_.min_threads;
    if (cfg_.scale_factor == 0) cfg_.scale_factor = 1;

    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < cfg_.min_threads; ++i) {
        SpawnLocked_();
    }
}

ThreadPool::~ThreadPool() {
    Shutdown();
}

size_t ThreadPool::QueueSize() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    return tasks_.size();
}

void ThreadPool::Enqueue_(Task t) {
    if (stopped_.load(std::memory_order_acquire)) {
        // fail-safe: synchronously execute so logs are not lost on static teardown
        try { t(); } catch (...) {}
        return;
    }

    bool need_spawn = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_.load(std::memory_order_relaxed)) {
            try { t(); } catch (...) {}
            return;
        }
        ReapLocked_();
        tasks_.push(std::move(t));

        const size_t wc = worker_count_.load(std::memory_order_relaxed);
        if (wc < cfg_.max_threads && tasks_.size() > wc * cfg_.scale_factor) {
            need_spawn = true;
        }
    }
    cv_.notify_one();
    if (need_spawn) {
        std::lock_guard<std::mutex> lk(mtx_);
        const size_t wc = worker_count_.load(std::memory_order_relaxed);
        if (!stopped_.load(std::memory_order_relaxed) &&
            wc < cfg_.max_threads &&
            tasks_.size() > wc * cfg_.scale_factor) {
            SpawnLocked_();
        }
    }
}

void ThreadPool::SpawnLocked_() {
    // worker_count_++ before construction; the new worker will see itself in workers_
    // only after this function returns, so the worker's self-erase path must wait
    // until then. We use a small handshake: insert thread::id later from inside
    // the worker. Instead, we just construct and emplace before incrementing visible count.
    std::thread t([this]() { WorkerLoop_(); });
    const auto id = t.get_id();
    workers_.emplace(id, std::move(t));
    worker_count_.fetch_add(1, std::memory_order_relaxed);
}

void ThreadPool::ReapLocked_() {
    for (auto& th : reap_) {
        if (th.joinable()) th.join();
    }
    reap_.clear();
}

void ThreadPool::WorkerLoop_() {
    for (;;) {
        Task task;
        bool got_task = false;
        bool should_exit = false;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            const bool timed_out = !cv_.wait_for(lk, cfg_.idle_timeout, [this]() {
                return stopped_.load(std::memory_order_relaxed) || !tasks_.empty();
            });

            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
                got_task = true;
            } else if (stopped_.load(std::memory_order_relaxed)) {
                should_exit = true;
            } else if (timed_out) {
                const size_t wc = worker_count_.load(std::memory_order_relaxed);
                if (wc > cfg_.min_threads) {
                    should_exit = true;
                }
            }

            if (should_exit) {
                auto it = workers_.find(std::this_thread::get_id());
                if (it != workers_.end()) {
                    reap_.push_back(std::move(it->second));
                    workers_.erase(it);
                }
                worker_count_.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        if (got_task) {
            try {
                task();
            } catch (const std::exception& e) {
                // Cannot route to logger itself — would recurse.
                std::fprintf(stderr, "[logger::context] task threw: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[logger::context] task threw unknown exception\n");
            }
        }

        if (should_exit) return;
    }
}

void ThreadPool::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_.exchange(true, std::memory_order_acq_rel)) {
            return;  // already stopped
        }
    }
    cv_.notify_all();

    // Move out the threads under lock, then join without holding the lock to
    // avoid blocking workers that try to self-erase on exit.
    std::unordered_map<std::thread::id, std::thread> taken;
    std::vector<std::thread> taken_reap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        taken.swap(workers_);
        taken_reap.swap(reap_);
    }
    for (auto& [id, th] : taken) {
        if (th.joinable()) th.join();
    }
    for (auto& th : taken_reap) {
        if (th.joinable()) th.join();
    }
}

}  // namespace logger::context
