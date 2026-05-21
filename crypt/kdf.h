#pragma once

#include <cstddef>
#include <string>

namespace logger::crypt {

// HKDF-SHA256 (RFC 5869). All inputs are raw bytes (may contain \0).
// out_len bytes of derived keying material are appended to out.
void HkdfSha256(const std::string& secret,
                const std::string& salt,
                const std::string& info,
                std::size_t        out_len,
                std::string&       out);

}  // namespace logger::crypt
