#include "handle/log_handle.h"

#include <utility>

namespace logger {

LogHandle::LogHandle(std::string name, sinks::LogSinkPtrList sinks)
    : name_(std::move(name)), sinks_(std::move(sinks)) {}

LogHandle::LogHandle(std::string name, sinks::LogSinkPtr sink)
    : name_(std::move(name)) {
    if (sink) {
        sinks_.push_back(std::move(sink));
    }
}

void LogHandle::Log(LogLevel level, SourceLocation loc, std::string_view message) {
    if (!ShouldLog(level)) {
        return;
    }
    const LogMsg msg{level, LogMsg::clock::now(), name_, loc, message};
    Log_(msg);
    if (ShouldFlush(level)) {
        Flush_();
    }
}

void LogHandle::Flush() {
    Flush_();
}

void LogHandle::Log_(const LogMsg& msg) {
    for (auto& sink : sinks_) {
        sink->Log(msg);
    }
}

void LogHandle::Flush_() {
    for (auto& sink : sinks_) {
        sink->Flush();
    }
}

}  // namespace logger
