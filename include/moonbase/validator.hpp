#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <nlohmann/json.hpp>

#include "moonbase/detail/base64.hpp"
#include "moonbase/detail/time.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint.hpp"
#include "moonbase/types.hpp"

namespace moonbase {

namespace detail {

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

inline std::vector<std::string> split_jwt(std::string_view token)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= token.size()) {
        const auto next = token.find('.', start);
        if (next == std::string_view::npos) {
            parts.emplace_back(token.substr(start));
            break;
        }
        parts.emplace_back(token.substr(start, next - start));
        start = next + 1;
    }

    if (parts.size() != 3 || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
        throw license_invalid_error("License token is malformed");
    }
    return parts;
}

inline std::string trim_ascii_whitespace(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

inline nlohmann::json decode_jwt_json(const std::string& encoded, const char* label)
{
    try {
        return nlohmann::json::parse(bytes_to_string(base64url_decode(encoded)));
    } catch (const std::exception& ex) {
        throw license_invalid_error(std::string("License token ") + label + " is malformed: " + ex.what());
    }
}

inline void verify_rs256(
    EVP_PKEY* pkey,
    const std::string& signing_input,
    const std::vector<unsigned char>& signature)
{
    evp_md_ctx_ptr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        throw license_invalid_error("Could not initialize signature verifier");
    }

    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey) != 1 ||
        EVP_DigestVerifyUpdate(ctx.get(), signing_input.data(), signing_input.size()) != 1 ||
        EVP_DigestVerifyFinal(ctx.get(), signature.data(), signature.size()) != 1) {
        throw license_invalid_error("License token signature is not valid");
    }
}

inline bool has_audience(const nlohmann::json& payload, const std::string& expected)
{
    if (!payload.contains("aud")) {
        return false;
    }
    const auto& aud = payload.at("aud");
    if (aud.is_string()) {
        return aud.get<std::string>() == expected;
    }
    if (aud.is_array()) {
        return std::any_of(aud.begin(), aud.end(), [&](const nlohmann::json& item) {
            return item.is_string() && item.get<std::string>() == expected;
        });
    }
    return false;
}

inline std::string require_string(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || !json.at(key).is_string()) {
        throw license_invalid_error(std::string("License token is missing string claim ") + key);
    }
    return json.at(key).get<std::string>();
}

inline std::optional<std::string> optional_string(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || json.at(key).is_null()) {
        return std::nullopt;
    }
    if (!json.at(key).is_string()) {
        throw license_invalid_error(std::string("License token claim is not a string: ") + key);
    }
    return json.at(key).get<std::string>();
}

inline bool require_boolish(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key)) {
        throw license_invalid_error(std::string("License token is missing boolean claim ") + key);
    }
    const auto& value = json.at(key);
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text == "true" || text == "True") {
            return true;
        }
        if (text == "false" || text == "False") {
            return false;
        }
    }
    throw license_invalid_error(std::string("License token claim is not boolean: ") + key);
}

inline long long require_integer(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || !json.at(key).is_number_integer()) {
        throw license_invalid_error(std::string("License token is missing integer claim ") + key);
    }
    return json.at(key).get<long long>();
}

inline std::optional<long long> optional_integer(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || json.at(key).is_null()) {
        return std::nullopt;
    }
    if (!json.at(key).is_number_integer()) {
        throw license_invalid_error(std::string("License token claim is not an integer: ") + key);
    }
    return json.at(key).get<long long>();
}

inline std::vector<std::string> split_claim_list(const std::optional<std::string>& value)
{
    std::vector<std::string> result;
    if (!value || value->empty()) {
        return result;
    }
    std::stringstream stream(*value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

inline std::chrono::system_clock::time_point require_validation_time(const nlohmann::json& payload)
{
    if (auto epoch = optional_integer(payload, "validated")) {
        return from_epoch_seconds(*epoch);
    }
    if (auto iso = optional_string(payload, "ver")) {
        return parse_iso8601_utc(*iso);
    }
    throw license_invalid_error("License token is missing validation timestamp");
}

inline nlohmann::json object_claim_or_empty(const nlohmann::json& payload, const char* key)
{
    if (!payload.contains(key) || payload.at(key).is_null()) {
        return nlohmann::json::object();
    }
    if (!payload.at(key).is_object()) {
        throw license_invalid_error(std::string("License token claim is not an object: ") + key);
    }
    return payload.at(key);
}

} // namespace detail

class license_validator {
public:
    license_validator(licensing_options options, std::shared_ptr<fingerprint_provider> fingerprints)
        : options_(std::move(options)),
          fingerprints_(std::move(fingerprints)),
          key_(detail::load_public_key(options_.public_key))
    {
        if (!fingerprints_) {
            throw configuration_error("A fingerprint provider is required");
        }
    }

    [[nodiscard]] license validate_token(std::string_view token) const
    {
        return validate_token_internal(token, false);
    }

    // Same as validate_token, but does not throw on a past `exp`. Intended for
    // operations like revoke where the seat may still be allocated server-side
    // even after the local token has aged out. All other checks (signature,
    // audience, issuer, device match) still apply.
    [[nodiscard]] license validate_token_allow_expired(std::string_view token) const
    {
        return validate_token_internal(token, true);
    }

private:
    [[nodiscard]] license validate_token_internal(std::string_view token, bool allow_expired) const
    {
        const auto token_string = detail::trim_ascii_whitespace(std::string(token));
        const auto parts = detail::split_jwt(token_string);
        const auto header = detail::decode_jwt_json(parts[0], "header");
        const auto payload = detail::decode_jwt_json(parts[1], "payload");

        if (!header.contains("alg") || header.at("alg") != "RS256") {
            throw license_invalid_error("License token algorithm is not supported");
        }

        const auto signing_input = parts[0] + "." + parts[1];
        detail::verify_rs256(
            key_.get(),
            signing_input,
            detail::base64url_decode(parts[2]));

        if (!detail::has_audience(payload, options_.product_id)) {
            throw license_invalid_error("License token audience does not match the configured product");
        }

        if (options_.account_id) {
            if (!payload.contains("iss") ||
                !payload.at("iss").is_string() ||
                payload.at("iss").get<std::string>() != *options_.account_id) {
                throw license_invalid_error("License token issuer does not match the configured account");
            }
        }

        license result;
        result.activation_id = detail::require_string(payload, "id");
        result.id = detail::require_string(payload, "l:id");
        result.trial = detail::require_boolish(payload, "trial");
        result.method = activation_method_from_string(detail::require_string(payload, "method"));
        result.issued_at = detail::from_epoch_seconds(detail::require_integer(payload, "iat"));
        if (auto exp = detail::optional_integer(payload, "exp")) {
            result.expires_at = detail::from_epoch_seconds(*exp);
        }
        result.validated_at = detail::require_validation_time(payload);
        result.token = token_string;

        result.licensed_product.id = detail::require_string(payload, "p:id");
        result.licensed_product.name = detail::require_string(payload, "p:name");
        result.licensed_product.current_release_version = detail::optional_string(payload, "p:rel");
        result.licensed_product.properties = detail::object_claim_or_empty(payload, "p:properties");

        result.owned_sub_product_ids = detail::split_claim_list(detail::optional_string(payload, "sp:owned"));
        result.subscription_id = detail::optional_string(payload, "s:id");

        result.issued_to.id = detail::require_string(payload, "u:id");
        result.issued_to.name = detail::require_string(payload, "u:name");
        result.issued_to.email = detail::require_string(payload, "u:email");
        result.issued_to.properties = detail::object_claim_or_empty(payload, "u:properties");
        result.properties = result.trial
            ? detail::object_claim_or_empty(payload, "t:properties")
            : detail::object_claim_or_empty(payload, "l:properties");

        if (!allow_expired && result.expires_at
            && *result.expires_at < std::chrono::system_clock::now()) {
            throw license_expired_error("License has expired");
        }

        const auto expected_signature = fingerprints_->device_id();
        const auto actual_signature = detail::require_string(payload, "sig");
        if (actual_signature != expected_signature) {
            throw license_invalid_error("License does not match the current device");
        }

        return result;
    }

    licensing_options options_;
    std::shared_ptr<fingerprint_provider> fingerprints_;
    detail::evp_pkey_ptr key_;
};

} // namespace moonbase
