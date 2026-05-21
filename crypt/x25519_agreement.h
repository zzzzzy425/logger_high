#pragma once

#include "crypt/cipher.h"

namespace logger::crypt {

class X25519KeyAgreement final : public IKeyAgreement {
public:
    static constexpr std::size_t kKeyBytes = 32;

    X25519KeyAgreement()  = default;
    ~X25519KeyAgreement() override = default;

    void DeriveSharedSecret(const std::string& peer_public,
                            std::string&       out_eph_public,
                            std::string&       out_shared) override;

    const char* Name() const noexcept override { return "X25519"; }
};

}  // namespace logger::crypt
