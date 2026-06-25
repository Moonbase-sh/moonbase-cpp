#pragma once

// Crypto backend selector for Moonbase's RS256 JWT verification and the device
// fingerprint hash. The backend is chosen at compile time:
//
//   * Default (no macro set): OpenSSL — keeps existing non-JUCE consumers and
//     the test suite byte-for-byte unchanged.
//   * Define MOONBASE_CRYPTO_NATIVE: use the OS-native backend — Security.framework
//     on Apple, CNG/bcrypt on Windows, system libcrypto (OpenSSL) on Linux. This
//     is what the JUCE module defines so it pulls in no third-party crypto.
//   * Or force a specific backend by defining MOONBASE_CRYPTO_BACKEND to one of
//     MOONBASE_CRYPTO_OPENSSL / MOONBASE_CRYPTO_APPLE / MOONBASE_CRYPTO_WINDOWS.
//
// Whichever backend is active provides, in namespace moonbase::detail::crypto:
//   std::array<unsigned char, 32> sha256_raw(const unsigned char*, std::size_t);
//   class rsa_public_key { rsa_public_key(const std::string&);
//                          void verify_rs256(std::string_view, const std::vector<unsigned char>&) const; };

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#define MOONBASE_CRYPTO_OPENSSL 1
#define MOONBASE_CRYPTO_APPLE 2
#define MOONBASE_CRYPTO_WINDOWS 3

#if !defined(MOONBASE_CRYPTO_BACKEND)
#if defined(MOONBASE_CRYPTO_NATIVE)
#if defined(__APPLE__)
#define MOONBASE_CRYPTO_BACKEND MOONBASE_CRYPTO_APPLE
#elif defined(_WIN32)
#define MOONBASE_CRYPTO_BACKEND MOONBASE_CRYPTO_WINDOWS
#else
#define MOONBASE_CRYPTO_BACKEND MOONBASE_CRYPTO_OPENSSL
#endif
#else
#define MOONBASE_CRYPTO_BACKEND MOONBASE_CRYPTO_OPENSSL
#endif
#endif

#if MOONBASE_CRYPTO_BACKEND == MOONBASE_CRYPTO_APPLE
#include "moonbase/detail/crypto/apple_backend.hpp"
#elif MOONBASE_CRYPTO_BACKEND == MOONBASE_CRYPTO_WINDOWS
#include "moonbase/detail/crypto/windows_backend.hpp"
#else
#include "moonbase/detail/crypto/openssl_backend.hpp"
#endif

namespace moonbase::detail {

// The RSA public key type the validator holds, resolved to the active backend.
using crypto::rsa_public_key;

// Lowercase hex SHA-256 of the material. The hex formatting lives here (shared)
// so the device-fingerprint output is byte-identical across all backends — any
// change would invalidate already-issued licenses whose `sig` claim was derived
// from the previous hash.
inline std::string sha256_hex(std::string_view material)
{
    const std::array<unsigned char, 32> digest = crypto::sha256_raw(
        reinterpret_cast<const unsigned char*>(material.data()), material.size());

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        out.push_back(hex[byte >> 4U]);
        out.push_back(hex[byte & 0x0FU]);
    }
    return out;
}

} // namespace moonbase::detail
