#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace logger::context {

class ThreadPool {
public:
    struct Config {
        size_t min_threads = 0;
        size_t max_threads = 0;
        size_t scale_factor = 2;
        std::chrono::milliseconds idle_timeout{30'000};

        static Config Default() noexcept;
    };

    explicit ThreadPool(Config cfg = Config::Default());
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    template <class F>
    void PostDetached(F&& f) {
        using Fn = std::decay_t<F>;
        if constexpr (std::is_copy_constructible_v<Fn>) {
            Enqueue_(Task(std::forward<F>(f)));
        } else {
            auto sp = std::make_shared<Fn>(std::forward<F>(f));
            Enqueue_([sp]() { (*sp)(); });
        }
    }

    template <class F, class R = std::invoke_result_t<std::decay_t<F>>>
    std::future<R> Submit(F&& f) {
        auto pt = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = pt->get_future();
        Enqueue_([pt]() { (*pt)(); });
        return fut;
    }

    void Shutdown();

    size_t WorkerCount() const noexcept { return worker_count_.load(std::memory_order_relaxed); }
    size_t QueueSize()  const noexcept;
    bool   Stopped()    const noexcept { return stopped_.load(std::memory_order_acquire); }

private:
    using Task = std::function<void()>;

    void Enqueue_(Task t);
    void SpawnLocked_();          // assumes mtx_ held
    void ReapLocked_();           // assumes mtx_ held
    void WorkerLoop_();

    Config                                            cfg_;
    mutable std::mutex                                mtx_;
    std::condition_variable                           cv_;
    std::queue<Task>                                  tasks_;
    std::unordered_map<std::thread::id, std::thread>  workers_;
    std::vector<std::thread>                          reap_;
    std::atomic<size_t>                               worker_count_{0};
    std::atomic<bool>                                 stopped_{false};
};

}  // namespace logger::context
