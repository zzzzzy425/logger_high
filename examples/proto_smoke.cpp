#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "formatter/effective_formatter.h"
#include "handle/log_msg.h"
#include "proto/log_record.pb.h"
#include "utils/log_level.h"
#include "utils/source_location.h"
#include "utils/system_info.h"

namespace {

logger::LogMsg MakeMsg(const char* name, const char* message,
                       bool with_loc = true) {
    logger::LogMsg m;
    m.level       = logger::LogLevel::info;
    m.time        = std::chrono::system_clock::now();
    m.logger_name = name;
    m.message     = message;
    if (with_loc) {
        m.loc = logger::SourceLocation{__FILE__, __LINE__,
                                       static_cast<const char*>(__func__)};
    } else {
        m.loc = logger::SourceLocation{};
    }
    return m;
}

void Case1_SerializeNonEmpty() {
    logger::formatter::EffectiveFormatter fmt;
    auto                                  m = MakeMsg("test1", "hello, world");
    std::string                           bytes;
    fmt.Format(m, bytes);
    assert(!bytes.empty());
    std::printf("[case1] serialize non-empty OK, %zu bytes\n", bytes.size());
}

void Case2_RoundTripFields() {
    logger::formatter::EffectiveFormatter fmt;
    auto                                  m = MakeMsg("audit", "user did X");
    std::string                           bytes;
    fmt.Format(m, bytes);

    logger::proto::LogRecord parsed;
    const bool               ok = parsed.ParseFromString(bytes);
    assert(ok);
    assert(parsed.level() == static_cast<std::uint32_t>(m.level));
    assert(parsed.logger_name() == "audit");
    assert(parsed.message() == "user did X");
    assert(parsed.pid() == logger::utils::GetPid());
    assert(parsed.tid() == logger::utils::GetTid());
    assert(parsed.line() == static_cast<std::uint32_t>(m.loc.line));
    assert(parsed.file() == std::string(m.loc.file));
    assert(parsed.func() == std::string(m.loc.func));

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        m.time.time_since_epoch())
                        .count();
    assert(parsed.time_ns() == ns);

    std::printf("[case2] field round-trip OK\n");
}

void Case3_UnicodeMessage() {
    logger::formatter::EffectiveFormatter fmt;
    // proto strings are UTF-8; pass UTF-8 bytes.
    const std::string utf8 = "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x9a\x80 ok";  // "中文 🚀 ok"
    auto              m   = MakeMsg("unicode", utf8.c_str());
    std::string       bytes;
    fmt.Format(m, bytes);

    logger::proto::LogRecord parsed;
    assert(parsed.ParseFromString(bytes));
    assert(parsed.message() == utf8);
    std::printf("[case3] UTF-8 round-trip OK\n");
}

void Case4_EmptySourceLocation() {
    logger::formatter::EffectiveFormatter fmt;
    auto m = MakeMsg("noloc", "no source info", /*with_loc=*/false);
    std::string bytes;
    fmt.Format(m, bytes);

    logger::proto::LogRecord parsed;
    assert(parsed.ParseFromString(bytes));
    assert(parsed.file().empty());
    assert(parsed.line() == 0);
    assert(parsed.func().empty());
    assert(parsed.message() == "no source info");
    std::printf("[case4] empty SourceLocation OK\n");
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    Case1_SerializeNonEmpty();
    Case2_RoundTripFields();
    Case3_UnicodeMessage();
    Case4_EmptySourceLocation();
    google::protobuf::ShutdownProtobufLibrary();
    std::printf("proto_smoke: all cases passed\n");
    return 0;
}
