#include "sinks/eviction_runner.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "utils/internal_log.h"
#include "utils/log_level.h"

namespace logger::sinks {

namespace {

constexpr std::string_view kPrefix = "log-";
constexpr std::string_view kSuffix = ".bin";

bool LooksLikeShard(const std::string& name) {
    return name.size() > kPrefix.size() + kSuffix.size() &&
           name.compare(0, kPrefix.size(), kPrefix) == 0 &&
           name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

}  // namespace

EvictionRunner::EvictionRunner(std::filesystem::path dir,
                               std::size_t cap_bytes,
                               std::chrono::seconds interval,
                               std::function<std::filesystem::path()> get_active_shard)
    : dir_(std::move(dir)),
      cap_bytes_(cap_bytes),
      interval_(interval),
      get_active_shard_(std::move(get_active_shard)) {}

EvictionRunner::~EvictionRunner() {
    Stop();
}

void EvictionRunner::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread([this] { RunLoop_(); });
}

void EvictionRunner::Stop() {
    if (!started_.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    started_.store(false);
}

void EvictionRunner::RunLoop_() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        cv_.wait_for(lk, interval_, [this] { return stop_; });
        if (stop_) {
            break;
        }
        lk.unlock();
        try {
            SweepOnce_();
        } catch (const std::exception& e) {
            utils::InternalLogF(LogLevel::warn, "EvictionRunner",
                                "sweep error: {}", e.what());
        } catch (...) {
            utils::InternalLog(LogLevel::warn, "EvictionRunner",
                               "sweep error: unknown exception");
        }
        lk.lock();
    }
}

void EvictionRunner::SweepOnce_() {
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec) || ec) {
        return;
    }

    struct Entry {
        std::filesystem::path path;
        std::string           filename;
        std::uintmax_t        size;
    };

    std::vector<Entry> entries;
    std::uintmax_t     total = 0;
    for (const auto& de : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) {
            return;
        }
        if (!de.is_regular_file(ec)) {
            continue;
        }
        auto name = de.path().filename().string();
        if (!LooksLikeShard(name)) {
            continue;
        }
        const auto sz = de.file_size(ec);
        if (ec) {
            continue;
        }
        entries.push_back(Entry{de.path(), std::move(name), sz});
        total += sz;
    }

    if (total <= cap_bytes_) {
        return;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.filename < b.filename; });

    const auto active = get_active_shard_ ? get_active_shard_()
                                          : std::filesystem::path{};
    std::error_code active_ec;
    const auto active_canon = active.empty()
                                  ? std::filesystem::path{}
                                  : std::filesystem::weakly_canonical(active, active_ec);

    for (auto& e : entries) {
        if (total <= cap_bytes_) {
            break;
        }
        std::error_code canon_ec;
        const auto cur_canon = std::filesystem::weakly_canonical(e.path, canon_ec);
        if (!active_canon.empty() && !canon_ec && cur_canon == active_canon) {
            continue;
        }
        std::error_code rm_ec;
        std::filesystem::remove(e.path, rm_ec);
        if (rm_ec) {
            utils::InternalLogF(LogLevel::warn, "EvictionRunner",
                                "remove failed {}: {}",
                                e.path.string(), rm_ec.message());
            continue;
        }
        total -= e.size;
    }
}

}  // namespace logger::sinks
