#pragma once

#include "formatter/formatter.h"

namespace logger::formatter {

// 固定格式：[YYYY-MM-DD HH:MM:SS.mmm] [level] [name] [pid:tid] message (file:line)
class DefaultFormatter final : public IFormatter {
public:
    void Format(const LogMsg& msg, std::string& out) override;
};

}  // namespace logger::formatter
