#include "context/strand.h"

#include <cstdio>
#include <exception>

namespace logger::context {

void Strand::Enqueue_(Task t) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push(std::move(t));
        if (!running_) {
            running_ = true;
            start = true;
        }
    }
    if (start) {
        auto self = shared_from_this();
        pool_.PostDetached([self]() { self->Drain_(); });
    }
}

void Strand::Drain_() {
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (tasks_.empty()) {
                running_ = false;
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[logger::context::strand] task threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[logger::context::strand] task threw unknown exception\n");
        }
    }
}

}  // namespace logger::context
