#include "crypt/kdf.h"

#include <stdexcept>
#include <vector>

#include <cryptopp/hkdf.h>
#include <cryptopp/sha.h>

namespace logger::crypt {

void HkdfSha256(const std::string& secret,
                const std::string& salt,
                const std::string& info,
                std::size_t        out_len,
                std::string&       out) {
    if (out_len == 0) {
        return;
    }
    std::vector<CryptoPP::byte> buf(out_len);
    try {
        CryptoPP::HKDF<CryptoPP::SHA256> hkdf;
        hkdf.DeriveKey(buf.data(), buf.size(),
                       reinterpret_cast<const CryptoPP::byte*>(secret.data()),
                       secret.size(),
                       reinterpret_cast<const CryptoPP::byte*>(salt.data()),
                       salt.size(),
                       reinterpret_cast<const CryptoPP::byte*>(info.data()),
                       info.size());
    } catch (const CryptoPP::Exception& e) {
        throw std::runtime_error(std::string("HkdfSha256: ") + e.what());
    }
    out.append(reinterpret_cast<const char*>(buf.data()), buf.size());
}

}  // namespace logger::crypt
