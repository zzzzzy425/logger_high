#pragma once

#include "compress/compressor.h"

namespace logger::compress {

class ZlibCompressor final : public ICompressor {
public:
    // level: 1 (fastest) ~ 9 (best); 6 is zlib's default
    explicit ZlibCompressor(int level = 6);

    void Compress(const std::string& in, std::string& out) override;
    void Decompress(const std::string& in, std::string& out) override;

    const char* Name() const noexcept override { return "zlib"; }

private:
    int level_;
};

}  // namespace logger::compress
