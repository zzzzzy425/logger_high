#include "formatter/effective_formatter.h"

#include <chrono>
#include <stdexcept>
#include <string>

#include "proto/log_record.pb.h"
#include "utils/system_info.h"

namespace logger::formatter {

void EffectiveFormatter::Format(const LogMsg& msg, std::string& out) {
    logger::proto::LogRecord record;
    record.set_level(static_cast<std::uint32_t>(msg.level));

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        msg.time.time_since_epoch())
                        .count();
    record.set_time_ns(static_cast<std::int64_t>(ns));

    record.set_logger_name(std::string(msg.logger_name));

    if (msg.loc.empty()) {
        record.set_file("");
        record.set_line(0);
        record.set_func("");
    } else {
        record.set_file(msg.loc.file != nullptr ? msg.loc.file : "");
        record.set_line(static_cast<std::uint32_t>(msg.loc.line));
        record.set_func(msg.loc.func != nullptr ? msg.loc.func : "");
    }

    record.set_message(std::string(msg.message));
    record.set_pid(utils::GetPid());
    record.set_tid(utils::GetTid());

    std::string body;
    if (!record.SerializeToString(&body)) {
        throw std::runtime_error("EffectiveFormatter: SerializeToString failed");
    }
    out.append(body);
}

}  // namespace logger::formatter
