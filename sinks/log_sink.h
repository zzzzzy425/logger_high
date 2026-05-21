#pragma once

#include <memory>
#include <vector>

#include "formatter/formatter.h"
#include "handle/log_msg.h"

namespace logger::sinks {

class LogSink {
public:
    virtual ~LogSink() = default;

    virtual void Log(const LogMsg& msg) = 0;
    virtual void Flush()                = 0;

    void SetFormatter(std::unique_ptr<formatter::IFormatter> formatter) {
        formatter_ = std::move(formatter);
    }

    LogSink()                          = default;
    LogSink(const LogSink&)            = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&)                 = delete;
    LogSink& operator=(LogSink&&)      = delete;

protected:
    std::unique_ptr<formatter::IFormatter> formatter_;
};

using LogSinkPtr     = std::shared_ptr<LogSink>;
using LogSinkPtrList = std::vector<LogSinkPtr>;

}  // namespace logger::sinks
