#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <type_traits>
#include <utility>

#include "context/thread_pool.h"

namespace logger::context {

class Strand : public std::enable_shared_from_this<Strand> {
public:
    static std::shared_ptr<Strand> Create(ThreadPool& pool) {
        return std::shared_ptr<Strand>(new Strand(pool));
    }

    Strand(const Strand&)            = delete;
    Strand& operator=(const Strand&) = delete;
    Strand(Strand&&)                 = delete;
    Strand& operator=(Strand&&)      = delete;

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

private:
    using Task = std::function<void()>;

    explicit Strand(ThreadPool& pool) : pool_(pool) {}

    void Enqueue_(Task t);
    void Drain_();

    ThreadPool&       pool_;
    std::mutex        mtx_;
    std::queue<Task>  tasks_;
    bool              running_{false};  // guarded by mtx_
};

}  // namespace logger::context
