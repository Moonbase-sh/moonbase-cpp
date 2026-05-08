#pragma once

#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <nlohmann/json.hpp>

#include "moonbase/detail/base64.hpp"
#include "moonbase/http.hpp"
#include "moonbase/types.hpp"

namespace moonbase::tests {

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using evp_pkey_ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using evp_md_ctx_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;

struct generated_key {
    evp_pkey_ptr key{nullptr, EVP_PKEY_free};
    std::string public_pem;
    std::string public_pkcs1_pem;
};

inline std::string bio_to_string(BIO* bio)
{
    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(bio, &memory);
    return {memory->data, memory->length};
}

inline generated_key generate_key()
{
    evp_pkey_ctx_ptr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!context ||
        EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0) {
        throw std::runtime_error("Could not initialize RSA key generation");
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw_key) <= 0) {
        throw std::runtime_error("Could not generate RSA key");
    }

    generated_key result;
    result.key.reset(raw_key);

    {
        bio_ptr bio(BIO_new(BIO_s_mem()), BIO_free);
        if (!bio || PEM_write_bio_PUBKEY(bio.get(), result.key.get()) != 1) {
            throw std::runtime_error("Could not export public key");
        }
        result.public_pem = bio_to_string(bio.get());
    }

    {
        bio_ptr bio(BIO_new(BIO_s_mem()), BIO_free);
        RSA* rsa = EVP_PKEY_get1_RSA(result.key.get());
        if (!bio || !rsa || PEM_write_bio_RSAPublicKey(bio.get(), rsa) != 1) {
            if (rsa) {
                RSA_free(rsa);
            }
            throw std::runtime_error("Could not export PKCS#1 public key");
        }
        RSA_free(rsa);
        result.public_pkcs1_pem = bio_to_string(bio.get());
    }

    return result;
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

inline std::vector<unsigned char> sign_rs256(EVP_PKEY* key, const std::string& input)
{
    evp_md_ctx_ptr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context ||
        EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSignUpdate(context.get(), input.data(), input.size()) != 1) {
        throw std::runtime_error("Could not initialize JWT signing");
    }

    std::size_t length = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &length) != 1) {
        throw std::runtime_error("Could not size JWT signature");
    }
    std::vector<unsigned char> signature(length);
    if (EVP_DigestSignFinal(context.get(), signature.data(), &length) != 1) {
        throw std::runtime_error("Could not sign JWT");
    }
    signature.resize(length);
    return signature;
}

inline std::string base64url_json(const nlohmann::json& json)
{
    const auto dumped = json.dump();
    return detail::base64url_encode(
        reinterpret_cast<const unsigned char*>(dumped.data()),
        dumped.size());
}

inline std::string make_token(
    EVP_PKEY* signing_key,
    nlohmann::json payload,
    nlohmann::json header = nlohmann::json{{"alg", "RS256"}, {"typ", "JWT"}})
{
    const auto signing_input = base64url_json(header) + "." + base64url_json(payload);
    const auto signature = sign_rs256(signing_key, signing_input);
    return signing_input + "." + detail::base64url_encode(signature);
}

inline long long now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline nlohmann::json default_claims(
    const std::string& device_id = "device-id",
    const std::string& product_id = "demo-app",
    const std::string& issuer = "tenant-1")
{
    const auto now = now_seconds();
    return {
        {"iss", issuer},
        {"aud", product_id},
        {"iat", now - 60},
        {"nbf", now - 60},
        {"exp", now + 3600},
        {"id", "activation-123"},
        {"l:id", "license-123"},
        {"trial", false},
        {"method", "Online"},
        {"sig", device_id},
        {"validated", now - 30},
        {"p:id", product_id},
        {"p:name", "Demo Product"},
        {"p:rel", "1.2.3"},
        {"sp:owned", "demo-app-pro,demo-app-extra"},
        {"s:id", "subscription-123"},
        {"u:id", "user-123"},
        {"u:name", "Jane Developer"},
        {"u:email", "jane@example.com"},
        {"p:properties", {{"tier", "pro"}}},
        {"u:properties", {{"company", "Acme"}}},
        {"l:properties", {{"seats", 3}}},
    };
}

class recording_transport : public http_transport {
public:
    std::vector<http_request> requests;
    std::deque<http_response> responses;

    explicit recording_transport(std::deque<http_response> queued = {})
        : responses(std::move(queued))
    {
    }

    http_response send(const http_request& request) override
    {
        requests.push_back(request);
        if (responses.empty()) {
            throw std::runtime_error("No mock HTTP response queued");
        }
        auto response = responses.front();
        responses.pop_front();
        return response;
    }
};

inline std::string getenv_or(const char* key, std::string fallback)
{
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return value;
}

} // namespace moonbase::tests
