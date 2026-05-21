#include "sinks/console_sink.h"

#include <iostream>
#include <string>

#include "formatter/default_formatter.h"

namespace logger::sinks {

ConsoleSink::ConsoleSink() {
    formatter_ = std::make_unique<formatter::DefaultFormatter>();
}

void ConsoleSink::Log(const LogMsg& msg) {
    std::string buf;
    buf.reserve(256);
    formatter_->Format(msg, buf);
    buf.push_back('\n');

    std::lock_guard<std::mutex> guard(mutex_);
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

void ConsoleSink::Flush() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::cout.flush();
}

}  // namespace logger::sinks
