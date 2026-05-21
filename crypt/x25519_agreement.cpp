#include "crypt/x25519_agreement.h"

#include <cstring>
#include <stdexcept>

#include <cryptopp/osrng.h>
#include <cryptopp/secblock.h>
#include <cryptopp/xed25519.h>

namespace logger::crypt {

void X25519KeyAgreement::DeriveSharedSecret(const std::string& peer_public,
                                            std::string&       out_eph_public,
                                            std::string&       out_shared) {
    if (peer_public.size() != kKeyBytes) {
        throw std::runtime_error(
            "X25519KeyAgreement: peer_public must be 32 bytes");
    }

    CryptoPP::AutoSeededRandomPool prng;
    CryptoPP::x25519               dh;

    CryptoPP::SecByteBlock eph_priv(kKeyBytes);
    CryptoPP::SecByteBlock eph_pub(kKeyBytes);
    CryptoPP::SecByteBlock shared(kKeyBytes);

    dh.GeneratePrivateKey(prng, eph_priv.BytePtr());
    dh.GeneratePublicKey(prng, eph_priv.BytePtr(), eph_pub.BytePtr());

    const bool ok = dh.Agree(
        shared.BytePtr(),
        eph_priv.BytePtr(),
        reinterpret_cast<const CryptoPP::byte*>(peer_public.data()),
        /*validateOtherPublicKey=*/true);

    // Wipe ephemeral private key as soon as agreement is done.
    std::memset(eph_priv.BytePtr(), 0, eph_priv.SizeInBytes());

    if (!ok) {
        std::memset(shared.BytePtr(), 0, shared.SizeInBytes());
        throw std::runtime_error("X25519KeyAgreement: Agree() failed");
    }

    out_eph_public.assign(reinterpret_cast<const char*>(eph_pub.BytePtr()),
                          kKeyBytes);
    out_shared.assign(reinterpret_cast<const char*>(shared.BytePtr()),
                      kKeyBytes);

    std::memset(shared.BytePtr(), 0, shared.SizeInBytes());
}

}  // namespace logger::crypt
