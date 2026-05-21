#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace logger::crypt {

inline constexpr std::size_t kServerPublicKeyBytes = 32;

// Reads exactly 32 raw bytes of the audit-party (server) X25519 public key.
// Throws std::runtime_error if the file does not exist or its size != 32.
std::string LoadServerPublicKey(const std::filesystem::path& path);

}  // namespace logger::crypt
