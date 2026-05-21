#pragma once

#include <cstdint>
#include <string_view>

namespace logger {

enum class LogLevel : std::uint8_t {
    trace    = 0,
    debug    = 1,
    info     = 2,
    warn     = 3,
    error    = 4,
    critical = 5,
    off      = 6,
};

constexpr std::string_view ToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace:    return "trace";
        case LogLevel::debug:    return "debug";
        case LogLevel::info:     return "info";
        case LogLevel::warn:     return "warn";
        case LogLevel::error:    return "error";
        case LogLevel::critical: return "critical";
        case LogLevel::off:      return "off";
    }
    return "unknown";
}

}  // namespace logger
