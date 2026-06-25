#pragma once

// Apple crypto backend — Security.framework + CommonCrypto. Used on macOS/iOS
// when MOONBASE_CRYPTO_NATIVE is set (the JUCE module), so the module needs no
// OpenSSL. RS256 verification uses SecKeyVerifySignature with the "Message"
// PKCS#1 v1.5 SHA-256 algorithm, which hashes the input internally.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>

#include "moonbase/detail/crypto/der.hpp"
#include "moonbase/errors.hpp"

namespace moonbase::detail::crypto {

inline std::array<unsigned char, 32> sha256_raw(const unsigned char* data, std::size_t length)
{
    std::array<unsigned char, 32> digest{};
    CC_SHA256(data, static_cast<CC_LONG>(length), digest.data());
    return digest;
}

namespace apple_detail {

// RAII for CoreFoundation handles (CFRelease on destruction).
template <typename T>
class cf_ref {
public:
    cf_ref() = default;
    explicit cf_ref(T ref) : ref_(ref) {}
    cf_ref(const cf_ref&) = delete;
    cf_ref& operator=(const cf_ref&) = delete;
    cf_ref(cf_ref&& other) noexcept : ref_(other.ref_) { other.ref_ = nullptr; }
    cf_ref& operator=(cf_ref&& other) noexcept
    {
        if (this != &other) {
            reset();
            ref_ = other.ref_;
            other.ref_ = nullptr;
        }
        return *this;
    }
    ~cf_ref() { reset(); }

    [[nodiscard]] T get() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }

private:
    void reset()
    {
        if (ref_ != nullptr) {
            CFRelease(ref_);
            ref_ = nullptr;
        }
    }
    T ref_ = nullptr;
};

inline cf_ref<CFDataRef> make_data(const unsigned char* bytes, std::size_t length)
{
    return cf_ref<CFDataRef>(
        CFDataCreate(kCFAllocatorDefault, bytes, static_cast<CFIndex>(length)));
}

} // namespace apple_detail

class rsa_public_key {
public:
    explicit rsa_public_key(const std::string& key_material)
    {
        const std::vector<unsigned char> pkcs1 = der::normalize_to_pkcs1(key_material);
        auto key_data = apple_detail::make_data(pkcs1.data(), pkcs1.size());
        if (!key_data) {
            throw license_invalid_error("Public key is not a supported RSA public key");
        }

        const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass};
        const void* values[] = {kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic};
        apple_detail::cf_ref<CFDictionaryRef> attributes(
            CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

        CFErrorRef error = nullptr;
        SecKeyRef key = SecKeyCreateWithData(key_data.get(), attributes.get(), &error);
        if (key == nullptr) {
            if (error != nullptr) {
                CFRelease(error);
            }
            throw license_invalid_error("Public key is not a supported RSA public key");
        }
        key_ = apple_detail::cf_ref<SecKeyRef>(key);
    }

    void verify_rs256(std::string_view signing_input,
                      const std::vector<unsigned char>& signature) const
    {
        auto message = apple_detail::make_data(
            reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size());
        auto sig = apple_detail::make_data(signature.data(), signature.size());
        if (!message || !sig) {
            throw license_invalid_error("License token signature is not valid");
        }

        CFErrorRef error = nullptr;
        const Boolean ok = SecKeyVerifySignature(
            key_.get(),
            kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
            message.get(),
            sig.get(),
            &error);
        if (error != nullptr) {
            CFRelease(error);
        }
        if (ok != true) {
            throw license_invalid_error("License token signature is not valid");
        }
    }

private:
    apple_detail::cf_ref<SecKeyRef> key_;
};

} // namespace moonbase::detail::crypto
