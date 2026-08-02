#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "moonbase/detail/base64.hpp"
#include "moonbase/detail/crypto/crypto.hpp"
#include "moonbase/detail/time.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/types.hpp"

namespace moonbase {

namespace detail {

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
        try {
            return parse_iso8601_utc(*iso);
        } catch (const std::exception& ex) {
            throw license_invalid_error(
                std::string("License token validation timestamp is malformed: ") + ex.what());
        }
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

// A factual note when the binding's stamp differs from what this SDK computes, or
// empty when it does not.
//
// Deliberately conditional about the machine. Reaching this point means nothing
// reproduced the bound id: a migrating_device_id_resolver, the only thing that
// could prove continuity, has already declined or was never configured. So the
// stamp says only which algorithm created the binding, never that it was *this*
// machine, because a token copied from another computer carries exactly the same
// relationship.
[[nodiscard]] inline std::string describe_version_difference(
    const fingerprint_spec::device_id_stamp& expected,
    const std::optional<fingerprint_spec::device_id_stamp>& bound)
{
    const auto expected_version = std::to_string(expected.version);

    // Direction matters. An older binding may need migrating; a newer one means
    // this SDK is behind, and re-activating would rebind the device to an
    // algorithm the issuing SDK has already moved on from.
    if (bound && bound->version > expected.version) {
        return "The binding was created by device fingerprint v" + std::to_string(bound->version)
            + ", which is newer than the v" + expected_version + " this SDK computes."
            + " Update the SDK rather than re-activating, which would rebind the device to the"
              " older algorithm.";
    }

    const auto bound_version = bound
        ? "device fingerprint v" + std::to_string(bound->version)
        : std::string("an SDK predating versioned device fingerprints");

    return "The binding was created by " + bound_version + ", while this SDK computes v"
        + expected_version + ", so this may instead be the same machine bound under the older"
          " algorithm. Re-activate to find out, or configure a migrating_device_id_resolver to keep"
          " accepting the previous id.";
}

// Same fingerprint version, different source tag: the two ids were built from
// different *kinds* of identity, so they were never going to match even on one
// machine. Saying only "not for this device" would point at the wrong remedy.
//
// Ordered by how badly a wrong message would mislead.
[[nodiscard]] inline std::string describe_source_difference(
    const fingerprint_spec::device_id_stamp& expected,
    const fingerprint_spec::device_id_stamp& bound)
{
    using source = fingerprint_spec::device_id_source;

    // A scoped id is stable only within one platform-defined scope. Comparing it
    // with anything from another scope is meaningless in both directions, so this
    // must not borrow the version path's "may be the same machine" phrasing: that
    // would be actively false.
    //
    // Which *side* is scoped decides the wording. A custom resolver or a native
    // bridge can make this SDK the scoped one, and saying "the binding is scoped"
    // there would describe the wrong id and point at the wrong remedy.
    if (bound.source == source::scoped) {
        return "The binding uses an app-scoped device identity, which cannot be compared with the id"
               " this SDK computes, not even on the same device. Re-activate here to bind this build.";
    }

    if (expected.source == source::scoped) {
        return "This SDK computes an app-scoped device identity, which cannot be compared with the one"
               " the binding carries, not even on the same device. Re-activate here to bind this app.";
    }

    // An unrecognised tag can only have come from a newer SDK. Before the parser
    // accepted arbitrary tags this fell through to the version branch and was
    // reported as predating versioned fingerprints, which was exactly backwards.
    if (!bound.source.has_value() || !expected.source.has_value()) {
        const auto& unknown = !bound.source.has_value() ? bound : expected;
        return "The binding carries the device identity tag \"" + unknown.source_tag
            + "\", which this SDK does not recognise. It was created by a newer Moonbase SDK, so"
              " update rather than re-activating.";
    }

    // Hardware identity versus the opt-in host-name fallback. Direction matters as
    // much as it does for versions, and the remedies are opposites.
    if (bound.source == source::device_name) {
        return "The binding was created from the host-name fallback, while this SDK reads hardware"
               " identity, so this may instead be the same machine bound while no hardware identity"
               " could be read. Re-activate to find out.";
    }

    return "The binding was created from hardware identity, while this SDK has fallen back to the"
           " host name. Check why hardware identity cannot be read here rather than re-activating,"
           " which would rebind the device to the weaker id.";
}

// The stamp difference behind a mismatch, or empty when there is none to report.
[[nodiscard]] inline std::string describe_stamp_difference(
    const std::string& expected,
    const std::string& bound)
{
    const auto expected_stamp = fingerprint_spec::parse_device_id_stamp(expected);
    if (!expected_stamp) {
        // A custom resolver's id, compared literally. Nothing to say about stamps.
        return {};
    }

    const auto bound_stamp = fingerprint_spec::parse_device_id_stamp(bound);

    if (!bound_stamp || bound_stamp->version != expected_stamp->version) {
        return describe_version_difference(*expected_stamp, bound_stamp);
    }

    if (bound_stamp->source_tag != expected_stamp->source_tag) {
        return describe_source_difference(*expected_stamp, *bound_stamp);
    }

    return {};
}

// Explain a `sig` mismatch. Leads with the only thing that is certain, that the
// bound id is not this device's, and appends the stamp difference when there is one.
[[nodiscard]] inline license_device_mismatch_error device_mismatch_error(
    const std::string& expected,
    const std::string& bound)
{
    const std::string detail = "This license is not for this device";
    const auto note = describe_stamp_difference(expected, bound);
    return license_device_mismatch_error(note.empty() ? detail : detail + ". " + note);
}

} // namespace detail

class license_validator {
public:
    license_validator(licensing_options options, std::shared_ptr<device_id_resolver> device_ids)
        : options_(std::move(options)),
          device_ids_(std::move(device_ids)),
          key_(options_.public_key)
    {
        if (!device_ids_) {
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
        std::vector<unsigned char> signature;
        try {
            signature = detail::base64url_decode(parts[2]);
        } catch (const std::exception& ex) {
            throw license_invalid_error(
                std::string("License token signature is malformed: ") + ex.what());
        }
        key_.verify_rs256(signing_input, signature);

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

        // Seat counts. The backend issues these as the claims p:seats:total /
        // p:seats:used (p:seats:remaining is derivable). Fall back to a couple of
        // alternative spellings and to the license properties object.
        result.seat_count = detail::optional_integer(payload, "p:seats:total");
        result.seats_used = detail::optional_integer(payload, "p:seats:used");
        if (!result.seat_count) {
            result.seat_count = detail::optional_integer(payload, "seats");
        }
        if (!result.seats_used) {
            result.seats_used = detail::optional_integer(payload, "seatsUsed");
        }
        const auto seat_from_properties = [&result](const char* key) -> std::optional<long long> {
            if (result.properties.contains(key) && result.properties.at(key).is_number_integer()) {
                return result.properties.at(key).get<long long>();
            }
            return std::nullopt;
        };
        if (!result.seat_count) {
            result.seat_count = seat_from_properties("seats");
        }
        if (!result.seats_used) {
            result.seats_used = seat_from_properties("seatsUsed");
        }

        if (!allow_expired && result.expires_at
            && *result.expires_at < std::chrono::system_clock::now()) {
            throw license_expired_error("License has expired");
        }

        const auto expected_signature = device_ids_->device_id();
        const auto actual_signature = detail::require_string(payload, "sig");
        // The literal comparison first, so an app that has not configured a
        // migration pays nothing. Only on a mismatch does a
        // migrating_device_id_resolver get the chance to vouch for an id this
        // machine used to be bound to, which is the one thing that can establish
        // continuity.
        if (actual_signature != expected_signature
            && !device_ids_->accepts_device_id(actual_signature)) {
            throw detail::device_mismatch_error(expected_signature, actual_signature);
        }

        return result;
    }

    licensing_options options_;
    std::shared_ptr<device_id_resolver> device_ids_;
    detail::rsa_public_key key_;
};

} // namespace moonbase
