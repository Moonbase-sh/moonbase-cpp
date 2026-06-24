#pragma once

// Windows crypto backend — CNG (bcrypt). Used on Windows when
// MOONBASE_CRYPTO_NATIVE is set (the JUCE module), so the module needs no
// OpenSSL. Unlike the Apple "Message" algorithm, BCryptVerifySignature wants a
// pre-computed SHA-256 digest plus a PKCS#1 padding descriptor.

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>

#include "moonbase/detail/crypto/der.hpp"
#include "moonbase/errors.hpp"

namespace moonbase::detail::crypto {

inline std::array<unsigned char, 32> sha256_raw(const unsigned char* data, std::size_t length)
{
    std::array<unsigned char, 32> digest{};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        throw license_invalid_error("Could not initialize signature verifier");
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptHashData(hash, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
                                static_cast<ULONG>(length), 0);
    }
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (!BCRYPT_SUCCESS(status)) {
        throw license_invalid_error("Could not initialize signature verifier");
    }
    return digest;
}

class rsa_public_key {
public:
    explicit rsa_public_key(const std::string& key_material)
    {
        const std::vector<unsigned char> pkcs1 = der::normalize_to_pkcs1(key_material);
        const der::rsa_components components = der::parse_rsa_pkcs1(pkcs1);

        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_RSA_ALGORITHM, nullptr, 0))) {
            throw license_invalid_error("Public key is not a supported RSA public key");
        }

        BCRYPT_RSAKEY_BLOB header{};
        header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        header.BitLength = static_cast<ULONG>(components.modulus.size() * 8U);
        header.cbPublicExp = static_cast<ULONG>(components.exponent.size());
        header.cbModulus = static_cast<ULONG>(components.modulus.size());
        header.cbPrime1 = 0;
        header.cbPrime2 = 0;

        std::vector<unsigned char> blob(sizeof(header) + components.exponent.size()
                                        + components.modulus.size());
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), components.exponent.data(),
                    components.exponent.size());
        std::memcpy(blob.data() + sizeof(header) + components.exponent.size(),
                    components.modulus.data(), components.modulus.size());

        const NTSTATUS status = BCryptImportKeyPair(
            algorithm_, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key_,
            blob.data(), static_cast<ULONG>(blob.size()), 0);
        if (!BCRYPT_SUCCESS(status)) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw license_invalid_error("Public key is not a supported RSA public key");
        }
    }

    rsa_public_key(const rsa_public_key&) = delete;
    rsa_public_key& operator=(const rsa_public_key&) = delete;
    rsa_public_key(rsa_public_key&&) = delete;
    rsa_public_key& operator=(rsa_public_key&&) = delete;

    ~rsa_public_key()
    {
        if (key_ != nullptr) {
            BCryptDestroyKey(key_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    void verify_rs256(std::string_view signing_input,
                      const std::vector<unsigned char>& signature) const
    {
        const std::array<unsigned char, 32> digest = sha256_raw(
            reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size());

        BCRYPT_PKCS1_PADDING_INFO padding{};
        padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;

        const NTSTATUS status = BCryptVerifySignature(
            key_, &padding,
            const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
            const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()),
            BCRYPT_PAD_PKCS1);
        if (!BCRYPT_SUCCESS(status)) {
            throw license_invalid_error("License token signature is not valid");
        }
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

} // namespace moonbase::detail::crypto
