#pragma once

#include <string>

#include "handle/log_msg.h"

namespace logger::formatter {

class IFormatter {
public:
    virtual ~IFormatter() = default;

    virtual void Format(const LogMsg& msg, std::string& out) = 0;

    IFormatter()                             = default;
    IFormatter(const IFormatter&)            = delete;
    IFormatter& operator=(const IFormatter&) = delete;
    IFormatter(IFormatter&&)                 = delete;
    IFormatter& operator=(IFormatter&&)      = delete;
};

}  // namespace logger::formatter
