#pragma once

// OpenSSL crypto backend — the default. Selected unless a different
// MOONBASE_CRYPTO_BACKEND is configured. This is the path existing non-JUCE
// consumers (and the test suite) compile, so its behavior and error messages
// are kept identical to the original inline implementation in validator.hpp.

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include "moonbase/detail/base64.hpp"
#include "moonbase/errors.hpp"

namespace moonbase::detail::crypto {

inline std::array<unsigned char, 32> sha256_raw(const unsigned char* data, std::size_t length)
{
    std::array<unsigned char, 32> digest{};
    SHA256(data, length, digest.data());
    return digest;
}

namespace openssl_detail {

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using evp_md_ctx_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

inline evp_pkey_ptr make_empty_pkey()
{
    return evp_pkey_ptr(nullptr, EVP_PKEY_free);
}

inline bio_ptr make_memory_bio(const std::string& value)
{
    return bio_ptr(BIO_new_mem_buf(value.data(), static_cast<int>(value.size())), BIO_free);
}

inline evp_pkey_ptr read_pem_public_key(const std::string& public_key)
{
    {
        auto bio = make_memory_bio(public_key);
        if (bio) {
            if (auto* pkey = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr)) {
                return evp_pkey_ptr(pkey, EVP_PKEY_free);
            }
        }
    }

    {
        auto bio = make_memory_bio(public_key);
        if (bio) {
            if (auto* rsa = PEM_read_bio_RSAPublicKey(bio.get(), nullptr, nullptr, nullptr)) {
                auto* pkey = EVP_PKEY_new();
                if (!pkey) {
                    RSA_free(rsa);
                    throw license_invalid_error("Could not allocate RSA public key");
                }
                if (EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
                    RSA_free(rsa);
                    EVP_PKEY_free(pkey);
                    throw license_invalid_error("Could not assign RSA public key");
                }
                return evp_pkey_ptr(pkey, EVP_PKEY_free);
            }
        }
    }

    return make_empty_pkey();
}

inline evp_pkey_ptr read_der_public_key(const std::vector<unsigned char>& der)
{
    const unsigned char* cursor = der.data();
    if (auto* pkey = d2i_PUBKEY(nullptr, &cursor, static_cast<long>(der.size()))) {
        return evp_pkey_ptr(pkey, EVP_PKEY_free);
    }

    cursor = der.data();
    if (auto* rsa = d2i_RSAPublicKey(nullptr, &cursor, static_cast<long>(der.size()))) {
        auto* pkey = EVP_PKEY_new();
        if (!pkey) {
            RSA_free(rsa);
            throw license_invalid_error("Could not allocate RSA public key");
        }
        if (EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
            RSA_free(rsa);
            EVP_PKEY_free(pkey);
            throw license_invalid_error("Could not assign RSA public key");
        }
        return evp_pkey_ptr(pkey, EVP_PKEY_free);
    }

    return make_empty_pkey();
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

inline evp_pkey_ptr load_public_key(const std::string& public_key)
{
    if (public_key.find("-----BEGIN") != std::string::npos) {
        auto pkey = read_pem_public_key(public_key);
        if (pkey) {
            return pkey;
        }
    }

    try {
        auto der = base64_decode(public_key);
        auto pkey = read_der_public_key(der);
        if (pkey) {
            return pkey;
        }
    } catch (const std::exception&) {
    }

    throw license_invalid_error("Public key is not a supported RSA public key");
}

} // namespace openssl_detail

class rsa_public_key {
public:
    explicit rsa_public_key(const std::string& key_material)
        : key_(openssl_detail::load_public_key(key_material))
    {
    }

    void verify_rs256(std::string_view signing_input,
                      const std::vector<unsigned char>& signature) const
    {
        openssl_detail::evp_md_ctx_ptr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!ctx) {
            throw license_invalid_error("Could not initialize signature verifier");
        }

        if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key_.get()) != 1 ||
            EVP_DigestVerifyUpdate(ctx.get(), signing_input.data(), signing_input.size()) != 1 ||
            EVP_DigestVerifyFinal(ctx.get(), signature.data(), signature.size()) != 1) {
            throw license_invalid_error("License token signature is not valid");
        }
    }

private:
    openssl_detail::evp_pkey_ptr key_;
};

} // namespace moonbase::detail::crypto
