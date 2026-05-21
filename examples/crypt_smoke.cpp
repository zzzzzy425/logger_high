#include <cassert>
#include <cstdio>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

#include <cryptopp/osrng.h>
#include <cryptopp/secblock.h>
#include <cryptopp/xed25519.h>

#include "crypt/aes_gcm_cipher.h"
#include "crypt/kdf.h"
#include "crypt/x25519_agreement.h"

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

void Case1_GcmRoundTrip() {
    const std::string key = MakeRandom(32, 1u);
    logger::crypt::AesGcmCipher enc(key);
    logger::crypt::AesGcmCipher dec(key);

    const std::string plaintext = "the quick brown fox jumps over the lazy dog";
    std::string       sealed;
    enc.Encrypt(plaintext, sealed);
    assert(sealed.size() == 12 + plaintext.size() + 16);

    std::string opened;
    dec.Decrypt(sealed, opened);
    assert(opened == plaintext);
    std::printf("[case1] AES-256-GCM round-trip OK (sealed=%zu)\n", sealed.size());
}

void Case2_TamperDetection() {
    const std::string key = MakeRandom(32, 2u);
    logger::crypt::AesGcmCipher enc(key);
    logger::crypt::AesGcmCipher dec(key);

    const std::string plaintext = "important audit record";
    std::string       sealed;
    enc.Encrypt(plaintext, sealed);

    sealed[20] ^= 0x01;  // flip a byte inside the ciphertext body

    std::string opened;
    bool        threw = false;
    try {
        dec.Decrypt(sealed, opened);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    assert(opened.empty());
    std::printf("[case2] tamper detection OK\n");
}

void Case3_NonceUniqueness() {
    const std::string key = MakeRandom(32, 3u);
    logger::crypt::AesGcmCipher enc(key);

    std::set<std::string> nonces;
    for (int i = 0; i < 1000; ++i) {
        std::string sealed;
        enc.Encrypt("same plaintext", sealed);
        std::string nonce(sealed.data(), 12);
        const auto  ins = nonces.insert(nonce);
        assert(ins.second);
    }
    std::printf("[case3] 1k unique nonces OK\n");
}

void Case4_X25519BasicShape() {
    // Validate that the API runs end-to-end and produces 32B outputs, and
    // that successive calls produce distinct ephemeral pubs (and therefore
    // distinct shared secrets against a fixed peer).
    logger::crypt::X25519KeyAgreement ka;

    // Generate a valid peer keypair so Agree() doesn't reject it.
    CryptoPP::AutoSeededRandomPool prng;
    CryptoPP::x25519               dh;
    CryptoPP::SecByteBlock         peer_priv(32), peer_pub(32);
    dh.GeneratePrivateKey(prng, peer_priv.BytePtr());
    dh.GeneratePublicKey(prng, peer_priv.BytePtr(), peer_pub.BytePtr());
    const std::string peer_pub_str(
        reinterpret_cast<const char*>(peer_pub.BytePtr()), 32);

    std::string eph1, shared1;
    ka.DeriveSharedSecret(peer_pub_str, eph1, shared1);
    assert(eph1.size() == 32);
    assert(shared1.size() == 32);

    std::string eph2, shared2;
    ka.DeriveSharedSecret(peer_pub_str, eph2, shared2);
    assert(eph2 != eph1);
    assert(shared2 != shared1);
    std::printf("[case4] X25519 derive lengths + freshness OK\n");
}

void Case5_HkdfDeterminism() {
    const std::string secret = MakeRandom(32, 5u);
    const std::string salt   = MakeRandom(16, 6u);
    const std::string info   = "logger-high aes-256-gcm";

    std::string a, b;
    logger::crypt::HkdfSha256(secret, salt, info, 32, a);
    logger::crypt::HkdfSha256(secret, salt, info, 32, b);
    assert(a == b);
    assert(a.size() == 32);

    std::string c;
    logger::crypt::HkdfSha256(MakeRandom(32, 7u), salt, info, 32, c);
    assert(c != a);
    std::printf("[case5] HKDF determinism + sensitivity OK\n");
}

// End-to-end production-shaped scenario: audit party holds a static X25519
// keypair generated offline; the logger has only the public half. At startup
// the logger derives a session AES key via ECDH+HKDF, encrypts records, and
// writes its ephemeral public into the file header. The audit party later
// combines its static private with that ephemeral public to recover the same
// shared secret -> AES key -> decrypts records.
void Case6_EndToEnd() {
    CryptoPP::AutoSeededRandomPool prng;
    CryptoPP::x25519               dh;
    CryptoPP::SecByteBlock         audit_priv(32);
    CryptoPP::SecByteBlock         audit_pub(32);
    dh.GeneratePrivateKey(prng, audit_priv.BytePtr());
    dh.GeneratePublicKey(prng, audit_priv.BytePtr(), audit_pub.BytePtr());
    const std::string audit_pub_str(
        reinterpret_cast<const char*>(audit_pub.BytePtr()), 32);

    // ---- logger side ----
    logger::crypt::X25519KeyAgreement ka;
    std::string                       eph_pub_logger, shared_logger;
    ka.DeriveSharedSecret(audit_pub_str, eph_pub_logger, shared_logger);

    std::string aes_key_logger;
    logger::crypt::HkdfSha256(shared_logger, /*salt=*/"",
                              "logger-high aes-256-gcm", 32, aes_key_logger);

    logger::crypt::AesGcmCipher cipher_logger(aes_key_logger);
    const std::string           record =
        "audit: user login at 2026-05-21T10:00:00Z";
    std::string sealed;
    cipher_logger.Encrypt(record, sealed);

    // ---- audit side ----
    CryptoPP::SecByteBlock shared_audit(32);
    const bool             ok = dh.Agree(
        shared_audit.BytePtr(),
        audit_priv.BytePtr(),
        reinterpret_cast<const CryptoPP::byte*>(eph_pub_logger.data()),
        true);
    assert(ok);

    const std::string shared_audit_str(
        reinterpret_cast<const char*>(shared_audit.BytePtr()), 32);
    assert(shared_audit_str == shared_logger);

    std::string aes_key_audit;
    logger::crypt::HkdfSha256(shared_audit_str, /*salt=*/"",
                              "logger-high aes-256-gcm", 32, aes_key_audit);
    assert(aes_key_audit == aes_key_logger);

    logger::crypt::AesGcmCipher cipher_audit(aes_key_audit);
    std::string                 recovered;
    cipher_audit.Decrypt(sealed, recovered);
    assert(recovered == record);

    std::printf("[case6] end-to-end ECDH + HKDF + AES-GCM OK\n");
}

}  // namespace

int main() {
    Case1_GcmRoundTrip();
    Case2_TamperDetection();
    Case3_NonceUniqueness();
    Case4_X25519BasicShape();
    Case5_HkdfDeterminism();
    Case6_EndToEnd();
    std::printf("crypt_smoke: all cases passed\n");
    return 0;
}
