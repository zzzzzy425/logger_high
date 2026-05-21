#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "crypt/cipher.h"

namespace logger::crypt {

class AesGcmCipher final : public ICipher {
public:
    static constexpr std::size_t kKeyBytes   = 32;  // AES-256
    static constexpr std::size_t kNonceBytes = 12;  // 96-bit GCM nonce
    static constexpr std::size_t kTagBytes   = 16;  // 128-bit auth tag

    // key must be exactly 32 bytes. Throws std::runtime_error otherwise.
    explicit AesGcmCipher(const std::string& key);
    ~AesGcmCipher() override;

    void Encrypt(const std::string& plaintext, std::string& out) override;
    void Decrypt(const std::string& ciphertext, std::string& out) override;

    const char* Name() const noexcept override { return "AES-256-GCM"; }

private:
    void BuildNonce_(std::array<std::uint8_t, kNonceBytes>& nonce);

    std::array<std::uint8_t, kKeyBytes> key_{};
    std::array<std::uint8_t, 8>         nonce_prefix_{};  // process-unique random
    std::atomic<std::uint32_t>          counter_{0};
};

}  // namespace logger::crypt
