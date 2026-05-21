#include "crypt/server_key_loader.h"

#include <fstream>
#include <stdexcept>
#include <string>

namespace logger::crypt {

std::string LoadServerPublicKey(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        throw std::runtime_error("server public key file does not exist: " +
                                 path.string());
    }
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("failed to stat server public key file: " +
                                 path.string());
    }
    if (sz != kServerPublicKeyBytes) {
        throw std::runtime_error(
            "server public key size mismatch (expect 32 raw bytes), got " +
            std::to_string(sz) + " at " + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open server public key file: " +
                                 path.string());
    }
    std::string out(kServerPublicKeyBytes, '\0');
    in.read(out.data(), static_cast<std::streamsize>(kServerPublicKeyBytes));
    if (static_cast<std::size_t>(in.gcount()) != kServerPublicKeyBytes) {
        throw std::runtime_error("short read on server public key file: " +
                                 path.string());
    }
    return out;
}

}  // namespace logger::crypt
