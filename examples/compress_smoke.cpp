#include <cassert>
#include <cstdio>
#include <random>
#include <string>

#include "compress/zlib_compressor.h"

namespace {

std::string MakeRandom(std::size_t n, std::uint32_t seed) {
    std::mt19937                    rng(seed);
    std::uniform_int_distribution<> dist(0, 255);
    std::string                     s;
    s.resize(n);
    for (auto& c : s) {
        c = static_cast<char>(dist(rng));
    }
    return s;
}

void Case1_RoundTripRandom() {
    logger::compress::ZlibCompressor zc;
    const std::string                in = MakeRandom(4096, 1u);
    std::string                      compressed;
    zc.Compress(in, compressed);
    assert(!compressed.empty());

    std::string decompressed;
    zc.Decompress(compressed, decompressed);
    assert(decompressed == in);
    std::printf("[case1] random 4KB round-trip OK, in=%zu compressed=%zu\n",
                in.size(), compressed.size());
}

void Case2_RoundTripLarge() {
    logger::compress::ZlibCompressor zc;
    const std::string                in = MakeRandom(1024 * 1024, 2u);  // 1 MiB
    std::string                      compressed;
    zc.Compress(in, compressed);

    std::string decompressed;
    zc.Decompress(compressed, decompressed);
    assert(decompressed == in);
    std::printf("[case2] random 1MiB round-trip OK, in=%zu compressed=%zu\n",
                in.size(), compressed.size());
}

void Case3_EdgeCases() {
    logger::compress::ZlibCompressor zc;

    // Empty input
    {
        std::string in;
        std::string compressed;
        zc.Compress(in, compressed);
        std::string decompressed;
        zc.Decompress(compressed, decompressed);
        assert(decompressed.empty());
    }

    // Single byte
    {
        std::string in("X");
        std::string compressed;
        zc.Compress(in, compressed);
        std::string decompressed;
        zc.Decompress(compressed, decompressed);
        assert(decompressed == in);
    }

    // Non-empty 'out' parameter should be appended to, not overwritten
    {
        std::string in("hello world");
        std::string compressed = "PREFIX:";
        zc.Compress(in, compressed);
        assert(compressed.size() > 7);
        assert(compressed.compare(0, 7, "PREFIX:") == 0);

        std::string tail(compressed, 7);
        std::string decompressed;
        zc.Decompress(tail, decompressed);
        assert(decompressed == in);
    }

    std::printf("[case3] edge cases OK\n");
}

void Case4_CompressionRatio() {
    logger::compress::ZlibCompressor zc;
    std::string                      in(64 * 1024, 'A');  // 64KiB of 'A'
    std::string                      compressed;
    zc.Compress(in, compressed);
    const double ratio = static_cast<double>(compressed.size()) / in.size();
    assert(ratio < 0.1);

    std::string decompressed;
    zc.Decompress(compressed, decompressed);
    assert(decompressed == in);
    std::printf("[case4] highly redundant 64KiB ratio=%.4f OK\n", ratio);
}

}  // namespace

int main() {
    Case1_RoundTripRandom();
    Case2_RoundTripLarge();
    Case3_EdgeCases();
    Case4_CompressionRatio();
    std::printf("compress_smoke: all cases passed\n");
    return 0;
}
