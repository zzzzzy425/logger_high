#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "utils/log_level.h"

namespace logger::utils {

void InternalLog(LogLevel lv, std::string_view tag, std::string msg) noexcept;

template <class... Args>
void InternalLogF(LogLevel lv, std::string_view tag,
                  fmt::format_string<Args...> f, Args&&... args) noexcept {
    try {
        InternalLog(lv, tag, fmt::format(f, std::forward<Args>(args)...));
    } catch (...) {
    }
}

}  // namespace logger::utils
