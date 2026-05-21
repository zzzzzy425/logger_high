#include "crypt/aes_gcm_cipher.h"

#include <cstring>
#include <limits>
#include <stdexcept>

#include <cryptopp/aes.h>
#include <cryptopp/filters.h>
#include <cryptopp/gcm.h>
#include <cryptopp/osrng.h>

namespace logger::crypt {

namespace {

CryptoPP::AutoSeededRandomPool& Prng() {
    static CryptoPP::AutoSeededRandomPool prng;
    return prng;
}

}  // namespace

AesGcmCipher::AesGcmCipher(const std::string& key) {
    if (key.size() != kKeyBytes) {
        throw std::runtime_error("AesGcmCipher: key must be 32 bytes (AES-256)");
    }
    std::memcpy(key_.data(), key.data(), kKeyBytes);
    Prng().GenerateBlock(nonce_prefix_.data(), nonce_prefix_.size());
}

AesGcmCipher::~AesGcmCipher() {
    // Best-effort key zeroization.
    volatile std::uint8_t* p = key_.data();
    for (std::size_t i = 0; i < kKeyBytes; ++i) {
        p[i] = 0;
    }
}

void AesGcmCipher::BuildNonce_(std::array<std::uint8_t, kNonceBytes>& nonce) {
    const std::uint32_t c = counter_.fetch_add(1, std::memory_order_relaxed);
    if (c == std::numeric_limits<std::uint32_t>::max()) {
        // Refuse to wrap; a new key must be agreed.
        throw std::runtime_error("AesGcmCipher: nonce counter exhausted");
    }
    std::memcpy(nonce.data(), nonce_prefix_.data(), 8);
    // big-endian counter for readability when inspecting raw bytes
    nonce[8]  = static_cast<std::uint8_t>((c >> 24) & 0xFF);
    nonce[9]  = static_cast<std::uint8_t>((c >> 16) & 0xFF);
    nonce[10] = static_cast<std::uint8_t>((c >> 8) & 0xFF);
    nonce[11] = static_cast<std::uint8_t>(c & 0xFF);
}

void AesGcmCipher::Encrypt(const std::string& plaintext, std::string& out) {
    std::array<std::uint8_t, kNonceBytes> nonce{};
    BuildNonce_(nonce);

    const auto written_before = out.size();
    out.append(reinterpret_cast<const char*>(nonce.data()), kNonceBytes);

    try {
        CryptoPP::GCM<CryptoPP::AES>::Encryption enc;
        enc.SetKeyWithIV(key_.data(), kKeyBytes, nonce.data(), kNonceBytes);

        std::string body;  // ciphertext || tag
        CryptoPP::AuthenticatedEncryptionFilter ef(
            enc,
            new CryptoPP::StringSink(body),
            /*putAAD=*/false,
            /*macSize=*/static_cast<int>(kTagBytes));
        ef.ChannelPut(CryptoPP::DEFAULT_CHANNEL,
                      reinterpret_cast<const CryptoPP::byte*>(plaintext.data()),
                      plaintext.size());
        ef.ChannelMessageEnd(CryptoPP::DEFAULT_CHANNEL);

        out.append(body);
    } catch (const CryptoPP::Exception& e) {
        out.resize(written_before);
        throw std::runtime_error(std::string("AesGcmCipher::Encrypt: ") + e.what());
    }
}

void AesGcmCipher::Decrypt(const std::string& ciphertext, std::string& out) {
    if (ciphertext.size() < kNonceBytes + kTagBytes) {
        throw std::runtime_error("AesGcmCipher::Decrypt: input too short");
    }
    const auto* in  = reinterpret_cast<const CryptoPP::byte*>(ciphertext.data());
    const auto  ct_len  = ciphertext.size() - kNonceBytes - kTagBytes;

    const auto written_before = out.size();
    try {
        CryptoPP::GCM<CryptoPP::AES>::Decryption dec;
        dec.SetKeyWithIV(key_.data(), kKeyBytes, in, kNonceBytes);

        CryptoPP::AuthenticatedDecryptionFilter df(
            dec,
            new CryptoPP::StringSink(out),
            CryptoPP::AuthenticatedDecryptionFilter::DEFAULT_FLAGS,
            static_cast<int>(kTagBytes));

        // body = ciphertext || tag
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL, in + kNonceBytes, ct_len + kTagBytes);
        df.ChannelMessageEnd(CryptoPP::DEFAULT_CHANNEL);

        if (!df.GetLastResult()) {
            out.resize(written_before);
            throw std::runtime_error("AesGcmCipher::Decrypt: auth tag verification failed");
        }
    } catch (const CryptoPP::Exception& e) {
        out.resize(written_before);
        throw std::runtime_error(std::string("AesGcmCipher::Decrypt: ") + e.what());
    }
}

}  // namespace logger::crypt
