#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace logger::sinks {

// Background daemon that periodically scans `dir` for shard files matching
// "log-*.bin", sums their sizes, and if the total exceeds `cap_bytes` removes
// the oldest shards (lexicographic order == chronological order by design)
// until the total is back under the cap. The currently active shard (returned
// by get_active_shard) is always skipped.
class EvictionRunner {
public:
    EvictionRunner(std::filesystem::path dir,
                   std::size_t cap_bytes,
                   std::chrono::seconds interval,
                   std::function<std::filesystem::path()> get_active_shard);
    ~EvictionRunner();

    EvictionRunner(const EvictionRunner&)            = delete;
    EvictionRunner& operator=(const EvictionRunner&) = delete;
    EvictionRunner(EvictionRunner&&)                 = delete;
    EvictionRunner& operator=(EvictionRunner&&)      = delete;

    void Start();
    void Stop();

private:
    void RunLoop_();
    void SweepOnce_();

    const std::filesystem::path                     dir_;
    const std::size_t                               cap_bytes_;
    const std::chrono::seconds                      interval_;
    const std::function<std::filesystem::path()>    get_active_shard_;

    std::thread                                     thread_;
    std::mutex                                      mu_;
    std::condition_variable                         cv_;
    std::atomic<bool>                               started_{false};
    bool                                            stop_{false};   // guarded by mu_
};

}  // namespace logger::sinks
