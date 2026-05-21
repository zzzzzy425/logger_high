#pragma once

#include <string>

namespace logger::compress {

class ICompressor {
public:
    virtual ~ICompressor() = default;

    virtual void Compress(const std::string& in, std::string& out)   = 0;
    virtual void Decompress(const std::string& in, std::string& out) = 0;

    virtual const char* Name() const noexcept = 0;

    ICompressor()                              = default;
    ICompressor(const ICompressor&)            = delete;
    ICompressor& operator=(const ICompressor&) = delete;
    ICompressor(ICompressor&&)                 = delete;
    ICompressor& operator=(ICompressor&&)      = delete;
};

}  // namespace logger::compress
