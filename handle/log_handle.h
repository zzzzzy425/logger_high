#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

#include "handle/log_msg.h"
#include "sinks/log_sink.h"
#include "utils/log_level.h"
#include "utils/source_location.h"

namespace logger {

class LogHandle {
public:
    explicit LogHandle(std::string name, sinks::LogSinkPtrList sinks);
    explicit LogHandle(std::string name, sinks::LogSinkPtr sink);

    template <typename It>
    LogHandle(std::string name, It begin, It end)
        : LogHandle(std::move(name), sinks::LogSinkPtrList(begin, end)) {}

    ~LogHandle() = default;

    LogHandle(const LogHandle&)            = delete;
    LogHandle& operator=(const LogHandle&) = delete;
    LogHandle(LogHandle&&)                 = delete;
    LogHandle& operator=(LogHandle&&)      = delete;

    void SetLevel(LogLevel level) noexcept {
        level_.store(level, std::memory_order_relaxed);
    }
    LogLevel GetLevel() const noexcept {
        return level_.load(std::memory_order_relaxed);
    }
    void SetFlushLevel(LogLevel level) noexcept {
        flush_level_.store(level, std::memory_order_relaxed);
    }
    LogLevel GetFlushLevel() const noexcept {
        return flush_level_.load(std::memory_order_relaxed);
    }
    const std::string& Name() const noexcept { return name_; }

    void Log(LogLevel level, SourceLocation loc, std::string_view message);
    void Flush();

    bool ShouldLog(LogLevel level) const noexcept {
        return level >= level_.load(std::memory_order_relaxed) && !sinks_.empty();
    }

protected:
    bool ShouldFlush(LogLevel level) const noexcept {
        return level >= flush_level_.load(std::memory_order_relaxed);
    }

    void Log_(const LogMsg& msg);
    void Flush_();

private:
    std::atomic<LogLevel>  level_{LogLevel::info};
    std::atomic<LogLevel>  flush_level_{LogLevel::off};
    std::string            name_;
    sinks::LogSinkPtrList  sinks_;
};

}  // namespace logger
