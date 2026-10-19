#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

// Seats across all of the customer's licenses for an add-on.
struct sub_product_seats {
    long long total = 0;
    long long used = 0;
    long long remaining = 0;
};

// One of the product's add-ons (sub-products, with a product ID like
// "my-app.drum-kit") that the merchant includes in its license, as the license
// reports it for this device. To decide whether to unlock it, use
// has_sub_product, which also checks expires_at.
struct sub_product {
    // The add-on's own part of its product ID: "drum-kit" for "my-app.drum-kit".
    std::string id;
    // Whether it may be unlocked on this device, as of when the license was
    // issued: the device holds a seat of it, or its trial has not run out. An
    // add-on the customer owns, but with every seat taken elsewhere, is listed
    // and not active.
    bool active = false;
    // A trial backs it rather than a license the customer owns.
    bool trial = false;
    // Its seat on this device was revoked, by the customer or the merchant, so
    // validating does not take it back: offer licensing::activate_sub_product.
    bool revoked = false;
    // When it ends. Empty when it never does.
    std::optional<std::chrono::system_clock::time_point> expires_at;
    // Empty in offline licenses and for trials.
    std::optional<sub_product_seats> seats;
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
    // Every add-on the customer owns, by its own part of its ID, whether or not
    // it is included in the license. Ownership only: it says nothing about this
    // device. For add-ons included in the license, see sub_products.
    std::vector<std::string> owned_sub_product_ids;
    // The add-ons included in the product's license that the customer owns or
    // trials, each with whether it may be unlocked on this device. Empty for a
    // trial license and for products without such add-ons.
    std::vector<sub_product> sub_products;
    std::optional<std::string> subscription_id;
    std::optional<long long> seat_count;  // total activation seats (from token claims), if provided
    std::optional<long long> seats_used;  // seats currently in use, if provided
    nlohmann::json properties = nlohmann::json::object();
    std::string token;
};

// The add-on as the license lists it, or nullptr when it doesn't. Takes the
// add-on's own part of its product ID ("drum-kit") or its whole ID
// ("my-app.drum-kit").
[[nodiscard]] inline const sub_product* find_sub_product(const license& value, std::string_view sub_product_id)
{
    const auto prefix = value.licensed_product.id + ".";
    if (sub_product_id.size() > prefix.size() && sub_product_id.substr(0, prefix.size()) == prefix) {
        sub_product_id.remove_prefix(prefix.size());
    }
    for (const auto& entry : value.sub_products) {
        if (entry.id == sub_product_id) {
            return &entry;
        }
    }
    return nullptr;
}

// Whether to unlock one of the product's add-ons on this device: the license
// lists it as active, and it has not expired since the license was issued, which
// matters for an offline license. Takes the add-on's own part of its product ID
// or its whole ID, as find_sub_product does.
[[nodiscard]] inline bool has_sub_product(
    const license& value,
    std::string_view sub_product_id,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    const auto* entry = find_sub_product(value, sub_product_id);
    return entry != nullptr && entry->active && (!entry->expires_at || *entry->expires_at > now);
}

struct activation_request {
    std::string id;
    std::string request_url;
    std::string browser_url;
    // The activation method this request was started with. Set client-side from
    // the argument to request_activation: the client API returns only id/request/
    // browser, so it is never read off the response.
    activation_method method = activation_method::online;
};

// How a price recurs: "OneOff", or "Recurring" with how often it renews
// ("Daily", "Weekly", "Monthly", "Quarterly" or "Yearly").
struct sub_product_recurrence {
    std::string type;
    std::string cycle_length; // empty for "OneOff"
};

// One way an add-on is sold.
struct sub_product_pricing {
    std::string id;
    std::string name;
    // What it costs before discounts, by currency code.
    std::map<std::string, double> original_price;
    // What it costs now, by currency code, after any discount.
    std::map<std::string, double> price;
    bool has_discount = false;
    std::optional<sub_product_recurrence> recurrence;
};

// The add-on, with its prices when it is for sale, and whether the customer has it.
struct sub_product_catalog_product {
    // The add-on's whole product ID: "my-app.drum-kit".
    std::string id;
    std::string name;
    std::string tagline;     // empty when none
    std::string description; // empty when none
    std::string website;     // empty when none
    std::string icon_url;    // empty when none
    bool owned = false;
    bool subscribed = false;
    // The price to show first. Empty when it is not for sale.
    std::optional<sub_product_pricing> default_variation;
    // Every way it is sold. Empty when it is not for sale.
    std::vector<sub_product_pricing> variations;
};

enum class sub_product_trial_status {
    // It offers trials, but not to this device now: it is not included in the
    // license, the customer owns it, the license is a trial or an offline license,
    // or the trial asks for a newsletter opt-in the customer has not given. Also
    // what a status this version does not know reads as.
    unavailable,
    // licensing::request_sub_product_trial can start one now.
    available,
    // The device's trial is running.
    active,
    // The device had its trial, and it has ended.
    expired,
};

// An add-on's trial on this device, or whether one can be started.
struct sub_product_trial_offer {
    sub_product_trial_status status = sub_product_trial_status::unavailable;
    // How long a trial lasts. Empty when its trials never expire.
    std::optional<int> number_of_days;
    // When the device's trial ends. Empty when it has none, or one that never expires.
    std::optional<std::chrono::system_clock::time_point> expires_at;
    // The trial needs the customer to opt in to the newsletter, which they have not.
    bool requires_newsletter_opt_in = false;
};

// One of the product's add-ons as licensing::list_sub_products reports it for
// this device.
struct sub_product_catalog_entry {
    // The add-on's own part of its product ID, as license::sub_products names it.
    std::string id;
    sub_product_catalog_product product;
    // Whether it is included in the product's license, so the license reports it.
    bool included_in_license = false;
    // Whether to unlock it on this device. For an add-on included in the
    // license, what has_sub_product would say. For any other, whether the
    // customer owns or subscribes to it.
    bool active = false;
    // Its seat on this device was revoked, so validating does not take it back:
    // offer licensing::activate_sub_product.
    bool revoked = false;
    // When the license or trial behind it ends, for one included in the license.
    std::optional<std::chrono::system_clock::time_point> expires_at;
    // Seats across the customer's licenses for it, for one included in the
    // license that they own.
    std::optional<sub_product_seats> seats;
    // Its trial on this device, or whether one can be started. Empty when it
    // offers none.
    std::optional<sub_product_trial_offer> trial;
    // Where the customer buys it, when it is for sale. Add "?variation={id}" for
    // one of its variations. Empty when it is not for sale.
    std::string buy_url;
};

// The product's add-ons that the customer can do something with: those they own
// or subscribe to, those on sale, and those whose trial can be started or is
// running on this device.
struct sub_product_catalog {
    // The currency to show prices in, as the store suggests it for the customer.
    std::string currency;
    std::vector<sub_product_catalog_entry> sub_products;
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
    //
    // Each layer appends its own space-separated segment rather than replacing
    // what is already there, so the header reads outermost-last:
    // "moonbase-cpp/4.3.1 moonbase-juce/4.3.1 (JUCE v8.0.4; macOS 15.2) HISE/4.1.0".
    // Control characters are stripped and the segment is capped before it reaches
    // the header (see detail::sanitize_client_info).
    std::optional<std::string> client_info;
    std::map<std::string, std::string> metadata;
    // Whether activating and validating the product also takes a seat of each
    // add-on included in its license that the customer owns, so the license's
    // sub_products report it. Turn it off to take seats yourself with
    // licensing::activate_sub_product.
    bool auto_activate_sub_products = true;
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

namespace detail {

// Lenient readers for the optional parts of an add-on, in a license file or an
// API answer. A part that is absent, null or of another type reads as not there,
// so a later release that adds to these shapes is no reason to fail.
inline std::string optional_text(const nlohmann::json& json, const char* key)
{
    return json.contains(key) && json.at(key).is_string() ? json.at(key).get<std::string>() : std::string();
}

inline bool optional_flag(const nlohmann::json& json, const char* key)
{
    return json.contains(key) && json.at(key).is_boolean() && json.at(key).get<bool>();
}

inline std::optional<sub_product_seats> optional_seats(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key) || !json.at(key).is_object()) {
        return std::nullopt;
    }
    const auto& seats = json.at(key);
    for (const char* part : {"total", "used", "remaining"}) {
        if (!seats.contains(part) || !seats.at(part).is_number()) {
            return std::nullopt;
        }
    }
    return sub_product_seats{
        seats.at("total").get<long long>(),
        seats.at("used").get<long long>(),
        seats.at("remaining").get<long long>(),
    };
}

// A time in an API answer, which must read if it is there at all: a time that
// can't be read is a broken answer, not a time that never comes.
inline std::optional<std::chrono::system_clock::time_point> optional_api_time(
    const nlohmann::json& json,
    const char* key)
{
    if (!json.contains(key) || json.at(key).is_null()) {
        return std::nullopt;
    }
    return parse_api_timestamp(json.at(key).get<std::string>());
}

inline std::map<std::string, double> price_by_currency(const nlohmann::json& json, const char* key)
{
    std::map<std::string, double> prices;
    for (const auto& [currency, amount] : json.at(key).items()) {
        if (amount.is_number()) {
            prices.emplace(currency, amount.get<double>());
        }
    }
    return prices;
}

} // namespace detail

inline void to_json(nlohmann::json& json, const sub_product& value)
{
    json = nlohmann::json{
        {"id", value.id},
        {"active", value.active},
        {"trial", value.trial},
        {"revoked", value.revoked},
    };
    if (value.expires_at) {
        json["expiresAt"] = detail::format_iso8601_utc(*value.expires_at);
    }
    if (value.seats) {
        json["seats"] = nlohmann::json{
            {"total", value.seats->total},
            {"used", value.seats->used},
            {"remaining", value.seats->remaining},
        };
    }
}

inline void from_json(const nlohmann::json& json, sub_product& value)
{
    json.at("id").get_to(value.id);
    value.active = detail::optional_flag(json, "active");
    value.trial = detail::optional_flag(json, "trial");
    value.revoked = detail::optional_flag(json, "revoked");
    if (json.contains("expiresAt") && json.at("expiresAt").is_string()) {
        value.expires_at = detail::parse_iso8601_utc(json.at("expiresAt").get<std::string>());
    } else {
        value.expires_at.reset();
    }
    value.seats = detail::optional_seats(json, "seats");
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
        {"subProducts", value.sub_products},
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
    // Absent in a license stored before add-ons were reported. An entry that
    // can't be read is skipped rather than failing the license, which a store
    // would then delete as corrupt.
    value.sub_products.clear();
    if (json.contains("subProducts") && json.at("subProducts").is_array()) {
        for (const auto& entry : json.at("subProducts")) {
            try {
                value.sub_products.push_back(entry.get<sub_product>());
            } catch (const std::exception&) {
            }
        }
    }
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

// The add-on list licensing::list_sub_products reads. Required parts throw, so a
// broken answer fails as a whole; optional ones are read leniently, and a trial
// status this version does not know reads as unavailable, since the server may
// add statuses.
inline void from_json(const nlohmann::json& json, sub_product_recurrence& value)
{
    json.at("type").get_to(value.type);
    value.cycle_length = detail::optional_text(json, "cycleLength");
}

inline void from_json(const nlohmann::json& json, sub_product_pricing& value)
{
    json.at("id").get_to(value.id);
    json.at("name").get_to(value.name);
    value.original_price = detail::price_by_currency(json, "originalPrice");
    value.price = detail::price_by_currency(json, "price");
    json.at("hasDiscount").get_to(value.has_discount);
    if (json.contains("recurrence") && json.at("recurrence").is_object()) {
        value.recurrence = json.at("recurrence").get<sub_product_recurrence>();
    } else {
        value.recurrence.reset();
    }
}

inline void from_json(const nlohmann::json& json, sub_product_catalog_product& value)
{
    json.at("id").get_to(value.id);
    json.at("name").get_to(value.name);
    value.tagline = detail::optional_text(json, "tagline");
    value.description = detail::optional_text(json, "description");
    value.website = detail::optional_text(json, "website");
    value.icon_url = detail::optional_text(json, "iconUrl");
    json.at("owned").get_to(value.owned);
    json.at("subscribed").get_to(value.subscribed);
    if (json.contains("defaultVariation") && !json.at("defaultVariation").is_null()) {
        value.default_variation = json.at("defaultVariation").get<sub_product_pricing>();
    } else {
        value.default_variation.reset();
    }
    value.variations.clear();
    if (json.contains("variations") && !json.at("variations").is_null()) {
        json.at("variations").get_to(value.variations);
    }
}

inline sub_product_trial_status sub_product_trial_status_from_string(const std::string& value)
{
    if (value == "Available") {
        return sub_product_trial_status::available;
    }
    if (value == "Active") {
        return sub_product_trial_status::active;
    }
    if (value == "Expired") {
        return sub_product_trial_status::expired;
    }
    return sub_product_trial_status::unavailable;
}

inline void from_json(const nlohmann::json& json, sub_product_trial_offer& value)
{
    value.status = sub_product_trial_status_from_string(detail::optional_text(json, "status"));
    if (json.contains("numberOfDays") && json.at("numberOfDays").is_number()) {
        value.number_of_days = json.at("numberOfDays").get<int>();
    } else {
        value.number_of_days.reset();
    }
    value.expires_at = detail::optional_api_time(json, "expiresAt");
    value.requires_newsletter_opt_in = detail::optional_flag(json, "requiresNewsletterOptIn");
}

inline void from_json(const nlohmann::json& json, sub_product_catalog_entry& value)
{
    json.at("id").get_to(value.id);
    json.at("product").get_to(value.product);
    json.at("includedInLicense").get_to(value.included_in_license);
    json.at("active").get_to(value.active);
    value.revoked = detail::optional_flag(json, "revoked");
    value.expires_at = detail::optional_api_time(json, "expiresAt");
    value.seats = detail::optional_seats(json, "seats");
    if (json.contains("trial") && json.at("trial").is_object()) {
        value.trial = json.at("trial").get<sub_product_trial_offer>();
    } else {
        value.trial.reset();
    }
    value.buy_url = detail::optional_text(json, "buyUrl");
}

inline void from_json(const nlohmann::json& json, sub_product_catalog& value)
{
    json.at("currency").get_to(value.currency);
    json.at("subProducts").get_to(value.sub_products);
}

} // namespace moonbase
