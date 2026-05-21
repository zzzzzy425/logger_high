#include "compress/zlib_compressor.h"

#include <stdexcept>
#include <string>

#include <zlib.h>

namespace logger::compress {

namespace {

[[noreturn]] void ThrowZlib(const char* op, int ret) {
    std::string msg = "ZlibCompressor: ";
    msg.append(op);
    msg.append(" failed, ret=");
    msg.append(std::to_string(ret));
    throw std::runtime_error(msg);
}

constexpr uInt kChunkLimit = 1u << 30;  // 1 GiB per deflate/inflate call

}  // namespace

ZlibCompressor::ZlibCompressor(int level) : level_(level) {
    if (level_ < 1 || level_ > 9) {
        throw std::runtime_error("ZlibCompressor: level out of range (1..9)");
    }
}

void ZlibCompressor::Compress(const std::string& in, std::string& out) {
    z_stream zs{};
    int      ret = ::deflateInit2(&zs, level_, Z_DEFLATED, /*windowBits=*/15,
                                  /*memLevel=*/8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        ThrowZlib("deflateInit2", ret);
    }

    const auto written_before = out.size();
    const auto bound          = ::deflateBound(&zs, static_cast<uLong>(in.size()));
    out.resize(written_before + bound);

    zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in  = (in.size() > kChunkLimit) ? kChunkLimit : static_cast<uInt>(in.size());
    auto in_left = in.size() - zs.avail_in;

    zs.next_out  = reinterpret_cast<Bytef*>(out.data() + written_before);
    zs.avail_out = (bound > kChunkLimit) ? kChunkLimit : static_cast<uInt>(bound);
    auto out_left = bound - zs.avail_out;

    int flush = (in_left == 0) ? Z_FINISH : Z_NO_FLUSH;
    for (;;) {
        ret = ::deflate(&zs, flush);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            ::deflateEnd(&zs);
            ThrowZlib("deflate", ret);
        }

        if (zs.avail_in == 0 && in_left > 0) {
            const uInt take = (in_left > kChunkLimit) ? kChunkLimit : static_cast<uInt>(in_left);
            zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()) +
                                                  (in.size() - in_left));
            zs.avail_in = take;
            in_left -= take;
            if (in_left == 0) {
                flush = Z_FINISH;
            }
        }
        if (zs.avail_out == 0 && out_left > 0) {
            const uInt take = (out_left > kChunkLimit) ? kChunkLimit : static_cast<uInt>(out_left);
            zs.next_out  = reinterpret_cast<Bytef*>(out.data() + out.size() - out_left);
            zs.avail_out = take;
            out_left -= take;
        }
    }

    const auto produced = zs.total_out;
    ::deflateEnd(&zs);
    out.resize(written_before + produced);
}

void ZlibCompressor::Decompress(const std::string& in, std::string& out) {
    z_stream zs{};
    int      ret = ::inflateInit2(&zs, /*windowBits=*/15);
    if (ret != Z_OK) {
        ThrowZlib("inflateInit2", ret);
    }

    const auto written_before = out.size();
    auto       in_left        = in.size();
    zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in  = (in_left > kChunkLimit) ? kChunkLimit : static_cast<uInt>(in_left);
    in_left     -= zs.avail_in;

    // Initial guess: 4x input, grow as needed.
    std::size_t out_cap = in.size() == 0 ? 64 : in.size() * 4;
    out.resize(written_before + out_cap);
    zs.next_out  = reinterpret_cast<Bytef*>(out.data() + written_before);
    zs.avail_out = (out_cap > kChunkLimit) ? kChunkLimit : static_cast<uInt>(out_cap);

    for (;;) {
        ret = ::inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            ::inflateEnd(&zs);
            ThrowZlib("inflate", ret);
        }

        if (zs.avail_in == 0 && in_left > 0) {
            const uInt take = (in_left > kChunkLimit) ? kChunkLimit : static_cast<uInt>(in_left);
            zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()) +
                                                  (in.size() - in_left));
            zs.avail_in = take;
            in_left -= take;
        }
        if (zs.avail_out == 0) {
            // Grow output buffer by 2x.
            const auto produced = zs.total_out;
            out_cap            *= 2;
            out.resize(written_before + out_cap);
            zs.next_out  = reinterpret_cast<Bytef*>(out.data() + written_before + produced);
            const auto remain = out_cap - produced;
            zs.avail_out = (remain > kChunkLimit) ? kChunkLimit : static_cast<uInt>(remain);
        }
    }

    const auto produced = zs.total_out;
    ::inflateEnd(&zs);
    out.resize(written_before + produced);
}

}  // namespace logger::compress
