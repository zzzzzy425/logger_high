#pragma once

#include "formatter/formatter.h"

namespace logger::formatter {

// Serializes LogMsg into a protobuf LogRecord byte string. The output is
// raw bytes (may contain \0); callers must use std::string::size() rather
// than c_str().
class EffectiveFormatter final : public IFormatter {
public:
    void Format(const LogMsg& msg, std::string& out) override;
};

}  // namespace logger::formatter
