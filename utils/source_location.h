#pragma once

namespace logger {

struct SourceLocation {
    const char* file = nullptr;
    int         line = 0;
    const char* func = nullptr;

    constexpr bool empty() const noexcept { return file == nullptr; }
};

}  // namespace logger

#define LOGGER_SOURCE_LOCATION() \
    ::logger::SourceLocation { __FILE__, __LINE__, static_cast<const char*>(__func__) }
