#pragma once

#include <mutex>

#include "sinks/log_sink.h"

namespace logger::sinks {

// 最小实现：std::cout 直写 + mutex 串行化。
// 构造时若未传 formatter，自动装一份 DefaultFormatter。
class ConsoleSink final : public LogSink {
public:
    ConsoleSink();

    void Log(const LogMsg& msg) override;
    void Flush() override;

private:
    std::mutex mutex_;
};

}  // namespace logger::sinks
