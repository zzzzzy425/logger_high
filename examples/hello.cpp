#include <memory>

#include "handle/log_handle.h"
#include "handle/log_macros.h"
#include "sinks/console_sink.h"

int main() {
    auto console = std::make_shared<logger::sinks::ConsoleSink>();
    logger::LogHandle log("hello", console);

    log.SetLevel(logger::LogLevel::trace);
    log.SetFlushLevel(logger::LogLevel::error);

    LOG_TRACE(log,    "trace line");
    LOG_DEBUG(log,    "debug line");
    LOG_INFO(log,     "info line");
    LOG_WARN(log,     "warn line");
    LOG_ERROR(log,    "error line");
    LOG_CRITICAL(log, "critical line");

    log.Flush();
    return 0;
}
