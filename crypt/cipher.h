#pragma once

#include <string>

namespace logger::crypt {

class ICipher {
public:
    virtual ~ICipher() = default;

    // plaintext -> append [12B nonce][ciphertext][16B tag] to out.
    virtual void Encrypt(const std::string& plaintext, std::string& out) = 0;

    // [12B nonce][ciphertext][16B tag] -> append plaintext to out.
    // Throws std::runtime_error if auth tag verification fails.
    virtual void Decrypt(const std::string& ciphertext, std::string& out) = 0;

    virtual const char* Name() const noexcept = 0;

    ICipher()                          = default;
    ICipher(const ICipher&)            = delete;
    ICipher& operator=(const ICipher&) = delete;
    ICipher(ICipher&&)                 = delete;
    ICipher& operator=(ICipher&&)      = delete;
};

class IKeyAgreement {
public:
    virtual ~IKeyAgreement() = default;

    // Generate an ephemeral key pair, perform ECDH with peer_public, and
    // return both the local ephemeral public key (to publish) and the
    // resulting shared secret (to feed into a KDF).
    //   peer_public:    peer's static public key (algorithm-specific length)
    //   out_eph_public: this side's ephemeral public key (written by callee)
    //   out_shared:     shared secret (written by callee)
    // Throws std::runtime_error on invalid peer key or agreement failure.
    virtual void DeriveSharedSecret(const std::string& peer_public,
                                    std::string&       out_eph_public,
                                    std::string&       out_shared) = 0;

    virtual const char* Name() const noexcept = 0;

    IKeyAgreement()                                = default;
    IKeyAgreement(const IKeyAgreement&)            = delete;
    IKeyAgreement& operator=(const IKeyAgreement&) = delete;
    IKeyAgreement(IKeyAgreement&&)                 = delete;
    IKeyAgreement& operator=(IKeyAgreement&&)      = delete;
};

}  // namespace logger::crypt
