#include "formatter/default_formatter.h"

#include <chrono>
#include <ctime>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "utils/system_info.h"

namespace logger::formatter {

namespace {

inline const char* BaseName(const char* path) noexcept {
    if (path == nullptr) {
        return "";
    }
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

}  // namespace

void DefaultFormatter::Format(const LogMsg& msg, std::string& out) {
    using namespace std::chrono;
    const auto secs   = time_point_cast<seconds>(msg.time);
    const auto millis = duration_cast<milliseconds>(msg.time - secs).count();
    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm_local{};
#if defined(_WIN32)
    ::localtime_s(&tm_local, &t);
#else
    ::localtime_r(&t, &tm_local);
#endif

    const char* file = msg.loc.empty() ? "" : BaseName(msg.loc.file);
    const int   line = msg.loc.empty() ? 0 : msg.loc.line;

    fmt::format_to(
        std::back_inserter(out),
        "[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}] [{}] [{}] [{}:{}] {} ({}:{})",
        tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
        tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
        static_cast<int>(millis),
        ToString(msg.level),
        msg.logger_name,
        utils::GetPid(), utils::GetTid(),
        msg.message,
        file, line);
}

}  // namespace logger::formatter
