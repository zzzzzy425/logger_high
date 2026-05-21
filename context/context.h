#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "context/strand.h"
#include "context/thread_pool.h"

namespace logger::context {

class Context {
public:
    static Context& Instance();

    // Optional: must be called before the first Pool()/Post/MakeStrand.
    // Calls after initialization are silently ignored.
    void Configure(ThreadPool::Config cfg);

    template <class F>
    void PostDetached(F&& f) {
        Pool().PostDetached(std::forward<F>(f));
    }

    template <class F>
    auto Submit(F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        return Pool().Submit(std::forward<F>(f));
    }

    std::shared_ptr<Strand> MakeStrand() {
        return Strand::Create(Pool());
    }

    ThreadPool& Pool();

    void Shutdown();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

private:
    Context()  = default;
    ~Context();

    void EnsurePool_();

    std::once_flag               init_flag_;
    std::mutex                   cfg_mtx_;
    ThreadPool::Config           pending_cfg_{ThreadPool::Config::Default()};
    bool                         configured_{false};
    std::unique_ptr<ThreadPool>  pool_;
};

}  // namespace logger::context
