#pragma once

#include <chrono>
#include <string_view>

#include "utils/log_level.h"
#include "utils/source_location.h"

namespace logger {

struct LogMsg {
    using clock      = std::chrono::system_clock;
    using time_point = clock::time_point;

    LogLevel         level;
    time_point       time;
    std::string_view logger_name;
    SourceLocation   loc;
    std::string_view message;

    LogMsg() = default;

    LogMsg(LogLevel lvl,
           time_point tp,
           std::string_view name,
           SourceLocation source,
           std::string_view msg) noexcept
        : level(lvl), time(tp), logger_name(name), loc(source), message(msg) {}
};

}  // namespace logger
