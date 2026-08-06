#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "moonbase/detail/time.hpp"
#include "moonbase/errors.hpp"

namespace moonbase {

enum class activation_method {
    online,
    offline,
};

enum class platform {
    unknown,
    windows,
    linux_os,
    mac,
};

inline std::string to_string(activation_method method)
{
    switch (method) {
        case activation_method::online:
            return "Online";
        case activation_method::offline:
            return "Offline";
    }
    return "Online";
}

inline activation_method activation_method_from_string(const std::string& value)
{
    if (value == "Online" || value == "online") {
        return activation_method::online;
    }
    if (value == "Offline" || value == "offline") {
        return activation_method::offline;
    }
    throw license_invalid_error("Unknown activation method: " + value);
}

inline std::string to_string(platform value)
{
    switch (value) {
        case platform::windows:
            return "Windows";
        case platform::linux_os:
            return "Linux";
        case platform::mac:
            return "Mac";
        case platform::unknown:
            return "Unknown";
    }
    return "Unknown";
}

inline platform current_platform()
{
#if defined(_WIN32)
    return platform::windows;
#elif defined(__APPLE__)
    return platform::mac;
#elif defined(__linux__)
    return platform::linux_os;
#else
    return platform::unknown;
#endif
}

struct product {
    std::string id;
    std::string name;
    std::optional<std::string> current_release_version;
    nlohmann::json properties = nlohmann::json::object();
};

struct user {
    std::string id;
    std::string name;
    std::string email;
    nlohmann::json properties = nlohmann::json::object();
};

struct license {
    std::string id;
    std::string activation_id;
    bool trial = false;
    activation_method method = activation_method::online;
    product licensed_product;
    user issued_to;
    std::chrono::system_clock::time_point issued_at{};
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::chrono::system_clock::time_point validated_at{};
    std::vector<std::string> owned_sub_product_ids;
    std::optional<std::string> subscription_id;
    std::optional<long long> seat_count;  // total activation seats (from token claims), if provided
    std::optional<long long> seats_used;  // seats currently in use, if provided
    nlohmann::json properties = nlohmann::json::object();
    std::string token;
};

struct activation_request {
    std::string id;
    std::string request_url;
    std::string browser_url;
    // The activation method this request was started with. Set client-side from
    // the argument to request_activation: the client API returns only id/request/
    // browser, so it is never read off the response.
    activation_method method = activation_method::online;
};

struct licensing_options {
    std::string endpoint;
    std::string product_id;
    std::string public_key;
    std::optional<std::string> account_id;
    platform target_platform = current_platform();
    std::optional<std::string> application_version;
    // Identifies a higher-level integration built on top of the SDK (e.g. the
    // JUCE module). Appended to the User-Agent after "moonbase-cpp/<version>" so
    // the server can tell which client made the request.
    std::optional<std::string> client_info;
    std::map<std::string, std::string> metadata;
    std::chrono::milliseconds http_connect_timeout{std::chrono::seconds{10}};
    std::chrono::milliseconds http_request_timeout{std::chrono::seconds{30}};
    std::chrono::seconds online_validation_grace_period{std::chrono::hours(24 * 7)};
    std::chrono::seconds online_validation_min_interval{std::chrono::minutes(5)};
};

inline void to_json(nlohmann::json& json, const product& value)
{
    json = nlohmann::json{
        {"id", value.id},
        {"name", value.name},
        {"properties", value.properties},
    };
    if (value.current_release_version) {
        json["currentReleaseVersion"] = *value.current_release_version;
    }
}

inline void from_json(const nlohmann::json& json, product& value)
{
    json.at("id").get_to(value.id);
    json.at("name").get_to(value.name);
    if (json.contains("currentReleaseVersion") && !json.at("currentReleaseVersion").is_null()) {
        value.current_release_version = json.at("currentReleaseVersion").get<std::string>();
    } else {
        value.current_release_version.reset();
    }
    value.properties = json.value("properties", nlohmann::json::object());
}

inline void to_json(nlohmann::json& json, const user& value)
{
    json = nlohmann::json{
        {"id", value.id},
        {"name", value.name},
        {"email", value.email},
        {"properties", value.properties},
    };
}

inline void from_json(const nlohmann::json& json, user& value)
{
    json.at("id").get_to(value.id);
    json.at("name").get_to(value.name);
    json.at("email").get_to(value.email);
    value.properties = json.value("properties", nlohmann::json::object());
}

inline void to_json(nlohmann::json& json, const license& value)
{
    json = nlohmann::json{
        {"id", value.id},
        {"activationId", value.activation_id},
        {"trial", value.trial},
        {"activationMethod", to_string(value.method)},
        {"product", value.licensed_product},
        {"issuedTo", value.issued_to},
        {"issuedAt", detail::format_iso8601_utc(value.issued_at)},
        {"validatedAt", detail::format_iso8601_utc(value.validated_at)},
        {"ownedSubProductIds", value.owned_sub_product_ids},
        {"properties", value.properties},
        {"token", value.token},
    };
    if (value.expires_at) {
        json["expiresAt"] = detail::format_iso8601_utc(*value.expires_at);
    }
    if (value.subscription_id) {
        json["subscriptionId"] = *value.subscription_id;
    }
    if (value.seat_count) {
        json["seatCount"] = *value.seat_count;
    }
    if (value.seats_used) {
        json["seatsUsed"] = *value.seats_used;
    }
}

inline void from_json(const nlohmann::json& json, license& value)
{
    json.at("id").get_to(value.id);
    json.at("activationId").get_to(value.activation_id);
    json.at("trial").get_to(value.trial);
    value.method = activation_method_from_string(json.at("activationMethod").get<std::string>());
    json.at("product").get_to(value.licensed_product);
    json.at("issuedTo").get_to(value.issued_to);
    value.issued_at = detail::parse_iso8601_utc(json.at("issuedAt").get<std::string>());
    if (json.contains("expiresAt") && !json.at("expiresAt").is_null()) {
        value.expires_at = detail::parse_iso8601_utc(json.at("expiresAt").get<std::string>());
    } else {
        value.expires_at.reset();
    }
    value.validated_at = detail::parse_iso8601_utc(json.at("validatedAt").get<std::string>());
    value.owned_sub_product_ids = json.value("ownedSubProductIds", std::vector<std::string>{});
    if (json.contains("subscriptionId") && !json.at("subscriptionId").is_null()) {
        value.subscription_id = json.at("subscriptionId").get<std::string>();
    } else {
        value.subscription_id.reset();
    }
    if (json.contains("seatCount") && !json.at("seatCount").is_null()) {
        value.seat_count = json.at("seatCount").get<long long>();
    } else {
        value.seat_count.reset();
    }
    if (json.contains("seatsUsed") && !json.at("seatsUsed").is_null()) {
        value.seats_used = json.at("seatsUsed").get<long long>();
    } else {
        value.seats_used.reset();
    }
    value.properties = json.value("properties", nlohmann::json::object());
    json.at("token").get_to(value.token);
}

inline void to_json(nlohmann::json& json, const activation_request& value)
{
    json = nlohmann::json{
        {"id", value.id},
        {"request", value.request_url},
        {"browser", value.browser_url},
        {"activationMethod", to_string(value.method)},
    };
}

inline void from_json(const nlohmann::json& json, activation_request& value)
{
    json.at("id").get_to(value.id);
    json.at("request").get_to(value.request_url);
    json.at("browser").get_to(value.browser_url);
    // Optional on purpose: this parses the API response, which carries only
    // id/request/browser. request_activation stamps the requested method
    // afterwards. The key is only present when round-tripping our own to_json.
    value.method = (json.contains("activationMethod") && json.at("activationMethod").is_string())
        ? activation_method_from_string(json.at("activationMethod").get<std::string>())
        : activation_method::online;
}

} // namespace moonbase
