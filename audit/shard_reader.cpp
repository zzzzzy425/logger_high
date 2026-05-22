#include "audit/shard_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include <cryptopp/cryptlib.h>
#include <cryptopp/secblock.h>
#include <cryptopp/xed25519.h>

#include "compress/zlib_compressor.h"
#include "crypt/aes_gcm_cipher.h"
#include "crypt/kdf.h"
#include "sinks/shard_writer.h"  // for kHeaderBytes

namespace fs = std::filesystem;

namespace logger::audit {

namespace {

constexpr std::array<unsigned char, 8> kMagic = {'L', 'G', 'R', 'H', 0x01, 0x00, 0x00, 0x00};
constexpr std::uint32_t kAlgoIdAesGcmX25519HkdfSha256 = 1;
constexpr std::size_t   kClientEphPubBytes           = 32;
constexpr std::size_t   kServerPrivBytes             = 32;
constexpr std::size_t   kFrameMinBytes               = 12 + 16;  // nonce + tag

std::uint32_t DecodeLEU32(const unsigned char* p) noexcept {
    return  static_cast<std::uint32_t>(p[0])        |
           (static_cast<std::uint32_t>(p[1]) << 8)  |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string DeriveAesKey(std::string_view server_priv,
                         const std::string& client_eph_pub) {
    if (server_priv.size() != kServerPrivBytes) {
        throw std::runtime_error("audit: server private key must be 32 bytes");
    }
    if (client_eph_pub.size() != kClientEphPubBytes) {
        throw std::runtime_error("audit: client ephemeral public key must be 32 bytes");
    }

    CryptoPP::x25519 dh;
    CryptoPP::SecByteBlock shared(kClientEphPubBytes);
    const bool ok = dh.Agree(
        shared.BytePtr(),
        reinterpret_cast<const CryptoPP::byte*>(server_priv.data()),
        reinterpret_cast<const CryptoPP::byte*>(client_eph_pub.data()),
        true);
    if (!ok) {
        throw std::runtime_error("audit: X25519 Agree failed (peer key invalid?)");
    }

    const std::string shared_str(reinterpret_cast<const char*>(shared.BytePtr()),
                                 kClientEphPubBytes);
    std::string aes_key;
    crypt::HkdfSha256(shared_str, /*salt=*/"", "logger-high aes-256-gcm", 32, aes_key);
    return aes_key;
}

bool IsShardFilename(const std::string& name) {
    return name.size() > 8 &&
           name.compare(0, 4, "log-") == 0 &&
           name.compare(name.size() - 4, 4, ".bin") == 0;
}

}  // namespace

ShardHeader ReadShardHeader(std::istream& in) {
    char hdr[sinks::ShardWriter::kHeaderBytes];
    in.read(hdr, sizeof(hdr));
    if (static_cast<std::size_t>(in.gcount()) != sizeof(hdr)) {
        throw std::runtime_error("audit: short shard header (file truncated?)");
    }
    if (std::memcmp(hdr, kMagic.data(), kMagic.size()) != 0) {
        throw std::runtime_error("audit: shard magic mismatch (not an LGRH shard)");
    }
    ShardHeader h;
    h.algo_id = DecodeLEU32(reinterpret_cast<const unsigned char*>(hdr + 8));
    if (h.algo_id != kAlgoIdAesGcmX25519HkdfSha256) {
        throw std::runtime_error("audit: unsupported algo_id " + std::to_string(h.algo_id));
    }
    h.client_eph_pub.assign(hdr + 12, kClientEphPubBytes);
    return h;
}

std::vector<proto::LogRecord> DecodeShardFile(const fs::path&  shard,
                                              std::string_view server_priv) {
    std::ifstream in(shard, std::ios::binary);
    if (!in.good()) {
        throw std::runtime_error("audit: cannot open shard " + shard.string());
    }

    const ShardHeader hdr = ReadShardHeader(in);
    const std::string aes_key = DeriveAesKey(server_priv, hdr.client_eph_pub);

    crypt::AesGcmCipher    cipher(aes_key);
    compress::ZlibCompressor decompressor;

    std::vector<proto::LogRecord> records;
    while (true) {
        unsigned char lenbuf[4];
        in.read(reinterpret_cast<char*>(lenbuf), 4);
        const auto got = in.gcount();
        if (got == 0) {
            break;  // clean EOF between frames
        }
        if (got != 4) {
            throw std::runtime_error("audit: truncated frame length");
        }

        const std::uint32_t flen = DecodeLEU32(lenbuf);
        if (flen < kFrameMinBytes) {
            throw std::runtime_error("audit: frame too small (corrupt or wrong format)");
        }

        std::string encrypted(flen, '\0');
        in.read(encrypted.data(), static_cast<std::streamsize>(flen));
        if (static_cast<std::uint32_t>(in.gcount()) != flen) {
            throw std::runtime_error("audit: short frame body read");
        }

        std::string compressed;
        cipher.Decrypt(encrypted, compressed);
        std::string proto_bytes;
        decompressor.Decompress(compressed, proto_bytes);

        proto::LogRecord rec;
        if (!rec.ParseFromString(proto_bytes)) {
            throw std::runtime_error("audit: protobuf parse failed");
        }
        records.push_back(std::move(rec));
    }
    return records;
}

std::vector<proto::LogRecord> DecodeShardDir(const fs::path& dir,
                                             std::string_view server_priv) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        throw std::runtime_error("audit: not a directory: " + dir.string());
    }

    std::vector<fs::path> shards;
    for (const auto& de : fs::directory_iterator(dir)) {
        if (!de.is_regular_file()) continue;
        if (IsShardFilename(de.path().filename().string())) {
            shards.push_back(de.path());
        }
    }
    std::sort(shards.begin(), shards.end());

    std::vector<proto::LogRecord> out;
    for (const auto& p : shards) {
        auto recs = DecodeShardFile(p, server_priv);
        out.insert(out.end(),
                   std::make_move_iterator(recs.begin()),
                   std::make_move_iterator(recs.end()));
    }
    return out;
}

}  // namespace logger::audit
