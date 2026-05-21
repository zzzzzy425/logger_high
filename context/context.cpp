#include "context/context.h"

namespace logger::context {

Context& Context::Instance() {
    static Context inst;  // C++11 magic statics: thread-safe initialization
    return inst;
}

Context::~Context() {
    Shutdown();
}

void Context::Configure(ThreadPool::Config cfg) {
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    if (configured_) return;  // already initialized; ignore
    pending_cfg_ = cfg;
}

void Context::EnsurePool_() {
    std::call_once(init_flag_, [this]() {
        ThreadPool::Config cfg;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            cfg = pending_cfg_;
            configured_ = true;
        }
        pool_ = std::make_unique<ThreadPool>(cfg);
    });
}

ThreadPool& Context::Pool() {
    EnsurePool_();
    return *pool_;
}

void Context::Shutdown() {
    if (pool_) {
        pool_->Shutdown();
    }
}

}  // namespace logger::context
