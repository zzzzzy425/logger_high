#include "utils/internal_log.h"

#include <cstdio>

#include <fmt/core.h>

namespace logger::utils {

namespace {
thread_local bool g_in_internal_log = false;
}

void InternalLog(LogLevel lv, std::string_view tag, std::string msg) noexcept {
    if (g_in_internal_log) {
        return;
    }
    g_in_internal_log = true;
    try {
        fmt::print(stderr, "[logger-high][{}][{}] {}\n",
                   ToString(lv), tag, msg);
        std::fflush(stderr);
    } catch (...) {
    }
    g_in_internal_log = false;
}

}  // namespace logger::utils
