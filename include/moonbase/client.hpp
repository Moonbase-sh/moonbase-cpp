#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "moonbase/detail/url.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/http.hpp"
#include "moonbase/types.hpp"
#include "moonbase/validator.hpp"

namespace moonbase {

namespace detail {

inline std::string version_string()
{
#ifdef MOONBASE_CPP_VERSION
    return MOONBASE_CPP_VERSION;
#else
    return "0.0.0";
#endif
}

// Every transport we ship assembles headers into a single line, so a CR or LF in
// a caller-supplied client_info would inject a header and an embedded NUL would
// silently truncate one. Control characters become spaces, runs of whitespace
// collapse, and the result is trimmed and capped, so the User-Agent stays a
// well-formed, bounded token list however many integration layers appended to it.
inline std::string sanitize_client_info(const std::string& value)
{
    // Generous for a stack of "Name/Version (comment)" segments, and well under
    // the per-line header limits proxies enforce: an oversized User-Agent fails
    // as an opaque 400/431 that is undebuggable from the field.
    constexpr std::string::size_type max_length = 256;

    std::string result;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7F || byte == ' ') {
            if (!result.empty() && result.back() != ' ') {
                result.push_back(' '); // leading runs drop, interior runs collapse
            }
        } else {
            result.push_back(character);
        }
        if (result.size() >= max_length) {
            break;
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

inline std::string request_path(const licensing_options& options)
{
    return trim_trailing_slashes(options.endpoint) +
        "/api/client/activations/" +
        url_encode(options.product_id) +
        "/request";
}

inline std::string validate_path(const licensing_options& options)
{
    return trim_trailing_slashes(options.endpoint) +
        "/api/client/licenses/" +
        url_encode(options.product_id) +
        "/validate";
}

inline std::string revoke_path(const licensing_options& options)
{
    return trim_trailing_slashes(options.endpoint) +
        "/api/client/licenses/" +
        url_encode(options.product_id) +
        "/revoke";
}

inline std::map<std::string, std::string> client_query(const licensing_options& options)
{
    std::map<std::string, std::string> query{{"format", "JWT"}};
    if (options.target_platform != platform::unknown) {
        query["platform"] = to_string(options.target_platform);
    }
    if (options.application_version) {
        query["appVersion"] = *options.application_version;
    }
    for (const auto& [key, value] : options.metadata) {
        if (!value.empty()) {
            query["meta[" + key + "]"] = value;
        }
    }
    return query;
}

inline std::map<std::string, std::string> default_headers(const licensing_options& options,
                                                          const std::string& content_type = {})
{
    std::string user_agent = "moonbase-cpp/" + version_string();
    if (options.client_info) {
        // Sanitise before the emptiness check: a segment that is only whitespace
        // or control characters must not leave a dangling separator behind.
        const auto client_info = sanitize_client_info(*options.client_info);
        if (!client_info.empty()) {
            user_agent += " " + client_info;
        }
    }
    std::map<std::string, std::string> headers{
        {"Accept", "application/json, application/jwt, text/plain"},
        {"User-Agent", user_agent},
        {"x-mb-client", "moonbase-cpp"},
    };
    if (!content_type.empty()) {
        headers["Content-Type"] = content_type;
    }
    return headers;
}

struct problem_details {
    std::string title;
    std::string detail;
    std::string error_type;
};

// An HTML or XML page where JSON or a token belongs: a proxy, CDN or captive
// portal answered instead of the API.
inline bool looks_like_markup(const std::string& body)
{
    const auto trimmed = trim_ascii_whitespace(body);
    return !trimmed.empty() && trimmed.front() == '<';
}

// The model-validation 400 carries no detail, only a field-to-messages map.
// One message (the first field by name) is what a developer needs to see.
inline std::string first_validation_error(const nlohmann::json& problem)
{
    if (!problem.contains("errors") || !problem.at("errors").is_object()) {
        return {};
    }
    for (const auto& entry : problem.at("errors").items()) {
        const auto& messages = entry.value();
        if (messages.is_array() && !messages.empty() && messages.front().is_string()) {
            const auto message = messages.front().get<std::string>();
            return entry.key().empty() ? message : entry.key() + ": " + message;
        }
    }
    return {};
}

inline problem_details parse_problem(const std::string& body)
{
    // Long enough for any message the API writes, short enough that an error
    // page nobody parsed cannot become an exception message.
    constexpr std::size_t max_plain_text_length = 200;

    problem_details result;
    if (body.empty()) {
        return result;
    }
    try {
        const auto problem = nlohmann::json::parse(body);
        if (problem.is_string()) {
            // BadRequest("...") on an endpoint that otherwise answers JSON, e.g.
            // "Trials can not be revoked".
            result.detail = problem.get<std::string>();
            return result;
        }
        if (!problem.is_object()) {
            return result;
        }
        if (problem.contains("title") && problem.at("title").is_string()) {
            result.title = problem.at("title").get<std::string>();
        }
        if (problem.contains("detail") && problem.at("detail").is_string()) {
            result.detail = problem.at("detail").get<std::string>();
        }
        if (result.detail.empty()) {
            result.detail = first_validation_error(problem);
        }
        if (result.detail.empty() && result.title.empty() && problem.contains("message") &&
            problem.at("message").is_string()) {
            // The API gateway's own 502 and 504 bodies, e.g. {"message":"Endpoint
            // request timed out"}. Worth quoting, but not the API's verdict.
            result.detail = problem.at("message").get<std::string>();
        }
        if (problem.contains("errorType") && problem.at("errorType").is_string()) {
            result.error_type = problem.at("errorType").get<std::string>();
        }
    } catch (const std::exception&) {
        // Not JSON. A short line of plain text is still worth quoting, since the
        // same bare-string answers come back as text/plain to some clients.
        const auto text = trim_ascii_whitespace(body);
        if (!text.empty() && text.size() <= max_plain_text_length && !looks_like_markup(text) &&
            text.find_first_of("\r\n") == std::string::npos) {
            result.detail = text;
        }
    }
    return result;
}

inline std::string problem_message(long status_code, const problem_details& problem)
{
    if (!problem.detail.empty()) {
        return problem.detail;
    }
    if (!problem.title.empty()) {
        return problem.title;
    }
    return "Moonbase API request failed with HTTP " + std::to_string(status_code);
}

// A Retry-After given in seconds, the form the rate limiter in front of the API
// sends with its 429. An HTTP-date is ignored rather than parsed: nothing in front
// of the API sends one.
inline std::optional<std::chrono::seconds> retry_after(const http_response& response)
{
    constexpr std::string_view name = "retry-after";
    for (const auto& [key, value] : response.headers) {
        if (key.size() != name.size() ||
            !std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            })) {
            continue;
        }
        const auto text = trim_ascii_whitespace(value);
        // Nine digits is over thirty years, so the conversion cannot overflow.
        if (text.empty() || text.size() > 9 ||
            !std::all_of(text.begin(), text.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            })) {
            return std::nullopt;
        }
        return std::chrono::seconds{std::stol(text)};
    }
    return std::nullopt;
}

// A closed store answers every request with 410 StoreClosed, whatever was asked,
// so every endpoint reads this before anything else. The errorType is Moonbase's
// own extension, which is what makes it safe to treat as final: a 410 from
// anything else carries none and stays an api_error.
inline void throw_if_store_closed(const problem_details& problem)
{
    if (problem.error_type == "StoreClosed") {
        throw store_closed_error(problem.detail.empty() ? "This store has closed." : problem.detail);
    }
}

[[noreturn]] inline void throw_api_error(const http_response& response, const problem_details& problem)
{
    throw api_error(
        static_cast<int>(response.status_code),
        problem_message(response.status_code, problem),
        problem.title,
        problem.detail,
        retry_after(response));
}

// The shared mapping for the license endpoints. A 400 is a definitive refusal of
// what was sent, so it becomes license_invalid_error (license_expired_error for an
// expired license); everything else, including rate limiting and 5xx failures,
// stays an api_error that is worth retrying.
//
// That includes a 403 or 404, although /validate and /revoke send those for a
// license that no longer exists or changed hands. Their body is stock ASP.NET
// ProblemDetails with no errorType, which a misrouted request gets from any other
// ASP.NET service too, and reading it as final would skip the offline grace
// period and lock a valid license over a routing fault.
[[noreturn]] inline void throw_for_problem(const http_response& response)
{
    const auto problem = parse_problem(response.body);
    throw_if_store_closed(problem);

    if (problem.error_type == "LicenseExpired") {
        throw license_expired_error(problem_message(response.status_code, problem));
    }

    if (response.status_code == 400) {
        // Older servers said "expired" without an errorType. Only trusted on a
        // 400, so a gateway's 5xx page that mentions an expired session can never
        // pass for a definitive license verdict.
        if (problem.title.find("expired") != std::string::npos ||
            problem.detail.find("expired") != std::string::npos) {
            throw license_expired_error(problem_message(response.status_code, problem));
        }
        throw license_invalid_error(problem_message(response.status_code, problem));
    }

    throw_api_error(response, problem);
}

// The inventory endpoints never judge the license: a token they cannot use just
// makes the request anonymous. Their 400s are about the product id, version or
// platform asked for, so every failure other than a closed store is an api_error
// carrying the status.
[[noreturn]] inline void throw_for_inventory_problem(const http_response& response)
{
    const auto problem = parse_problem(response.body);
    throw_if_store_closed(problem);
    throw_api_error(response, problem);
}

// A 400 from the activation poll means the request will never complete: it
// expired, was cancelled, or was refused. Kept away from throw_for_problem's
// text match, which would report "Activation request has expired" as an expired
// license; only an explicit LicenseExpired code still means that.
[[noreturn]] inline void throw_for_activation_poll_problem(const http_response& response)
{
    if (response.status_code != 400) {
        throw_for_problem(response);
    }

    const auto problem = parse_problem(response.body);
    throw_if_store_closed(problem);
    const auto& reason = !problem.detail.empty() ? problem.detail : problem.title;
    if (problem.error_type == "LicenseExpired") {
        throw license_expired_error(reason.empty() ? "The license has expired" : reason);
    }
    throw activation_request_error(
        reason.empty() ? "Activation request can no longer be completed (HTTP 400)"
                       : "Activation request can no longer be completed: " + reason);
}

// A 2xx whose body is not a JWT at all did not come from the Moonbase API: a
// captive portal or a proxy answered in its place, usually with a web page. That
// says nothing about the license, so it must not reach the validator, whose
// license_invalid_error would skip the offline grace period and lock out a user
// who merely joined a network that intercepts traffic.
inline void ensure_token_response(const http_response& response)
{
    try {
        const auto parts = split_jwt(trim_ascii_whitespace(response.body));
        if (decode_jwt_json(parts[0], "header").is_object()) {
            return;
        }
    } catch (const std::exception&) {
    }
    throw api_error(
        static_cast<int>(response.status_code),
        "Moonbase API answered HTTP " + std::to_string(response.status_code) +
            " without a license token; a proxy or captive portal may have intercepted the request");
}

// Hard floor between activation polls, protecting the API from callers that poll
// from a fast UI timer or a tight loop. Deliberately not configurable.
inline constexpr std::chrono::milliseconds activation_poll_min_interval{1000};

// The longest Retry-After the poll honours, so a bad header cannot stall an
// activation for good.
inline constexpr std::chrono::seconds activation_poll_max_backoff{300};

// Spaces polls of the same activation request at least
// activation_poll_min_interval apart. Each request has its own slot, so a caller
// polling several requests in turn never starves the later ones. The timestamp
// is taken before the request goes out, so a poll still in flight on another
// thread counts too.
//
// Also honours a Retry-After an earlier poll was told.
class activation_poll_gate {
public:
    using clock = std::chrono::steady_clock;

    bool try_claim(const std::string& request_url, clock::time_point now = clock::now())
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (now < hold_until_) {
            return false;
        }
        // Polls older than the interval no longer hold anything back. Dropping
        // them keeps the map to the requests polled within the last interval.
        for (auto it = last_poll_.begin(); it != last_poll_.end();) {
            if (now - it->second >= activation_poll_min_interval) {
                it = last_poll_.erase(it);
            } else {
                ++it;
            }
        }
        return last_poll_.emplace(request_url, now).second;
    }

    // Holds back every poll until the server's Retry-After has passed. One hold
    // covers all requests, because the rate limit in front of the API counts
    // requests per client address, not per activation request.
    void back_off(std::chrono::seconds retry_after, clock::time_point now = clock::now())
    {
        std::lock_guard<std::mutex> guard(mutex_);
        // Parenthesised so <windows.h>'s min/max macros cannot expand them.
        hold_until_ = (std::max)(hold_until_, now + (std::min)(retry_after, activation_poll_max_backoff));
    }

private:
    std::mutex mutex_;
    std::map<std::string, clock::time_point> last_poll_;
    clock::time_point hold_until_{};
};

} // namespace detail

class license_client {
public:
    license_client(
        licensing_options options,
        std::shared_ptr<device_id_resolver> device_ids,
        std::shared_ptr<license_validator> validator,
        std::shared_ptr<http_transport> transport)
        : options_(std::move(options)),
          device_ids_(std::move(device_ids)),
          validator_(std::move(validator)),
          transport_(std::move(transport))
    {
        if (!device_ids_) {
            throw configuration_error("A fingerprint provider is required");
        }
        if (!validator_) {
            throw configuration_error("A license validator is required");
        }
        if (!transport_) {
            throw configuration_error("An HTTP transport is required");
        }
    }

    [[nodiscard]] activation_request request_activation(
        activation_method method = activation_method::online) const
    {
        // Only the request endpoint accepts "method"; client_query is shared
        // with /validate and /revoke, which do not, so add it here.
        auto query = detail::client_query(options_);
        query["method"] = to_string(method);
        const auto url = detail::append_query(detail::request_path(options_), query);

        // The API refuses a blank device name or id outright. The name is only a
        // label, so stand in for one the host could not supply (a failed host
        // name lookup, a custom resolver). The id is the binding itself, and a
        // resolver that returns none is misconfigured.
        auto device_name = device_ids_->device_name();
        if (detail::trim_ascii_whitespace(device_name).empty()) {
            device_name = "Unknown device";
        }
        const auto device_id = device_ids_->device_id();
        if (detail::trim_ascii_whitespace(device_id).empty()) {
            throw configuration_error("The device id resolver returned an empty device id");
        }

        const auto payload = nlohmann::json{
            {"deviceName", device_name},
            {"deviceSignature", device_id},
        };

        http_request request;
        request.method = "POST";
        request.url = url;
        request.headers = detail::default_headers(options_, "application/json");
        request.connect_timeout = options_.http_connect_timeout;
        request.request_timeout = options_.http_request_timeout;
        request.body = payload.dump();

        const auto response = transport_->send(request);
        if (response.status_code < 200 || response.status_code >= 300) {
            detail::throw_for_problem(response);
        }

        try {
            auto result = nlohmann::json::parse(response.body).get<activation_request>();
            // The response carries only id/request/browser, so record what we
            // asked for rather than leaving the default.
            result.method = method;
            return result;
        } catch (const std::exception& ex) {
            throw api_error(
                static_cast<int>(response.status_code),
                std::string("Could not parse activation response: ") + ex.what());
        }
    }

    [[nodiscard]] license validate_token_online(std::string_view token) const
    {
        const auto url = detail::append_query(
            detail::validate_path(options_),
            detail::client_query(options_));

        http_request request;
        request.method = "POST";
        request.url = url;
        request.headers = detail::default_headers(options_, "text/plain");
        request.connect_timeout = options_.http_connect_timeout;
        request.request_timeout = options_.http_request_timeout;
        request.body = std::string(token);

        const auto response = transport_->send(request);
        if (response.status_code < 200 || response.status_code >= 300) {
            detail::throw_for_problem(response);
        }
        detail::ensure_token_response(response);
        return validator_->validate_token(response.body);
    }

    void revoke_activation(std::string_view token) const
    {
        const auto url = detail::append_query(
            detail::revoke_path(options_),
            detail::client_query(options_));

        http_request request;
        request.method = "POST";
        request.url = url;
        request.headers = detail::default_headers(options_, "text/plain");
        request.connect_timeout = options_.http_connect_timeout;
        request.request_timeout = options_.http_request_timeout;
        request.body = std::string(token);

        const auto response = transport_->send(request);
        if (response.status_code < 200 || response.status_code >= 300) {
            detail::throw_for_problem(response);
        }
        // Success is an empty 200. A web page in its place means something in
        // between answered, and the seat may well still be taken.
        if (detail::looks_like_markup(response.body)) {
            throw api_error(
                static_cast<int>(response.status_code),
                "Moonbase API answered HTTP " + std::to_string(response.status_code) +
                    " with a web page; a proxy or captive portal may have intercepted the request");
        }
    }

    // Returns nullopt while the activation is pending, and also, without contacting
    // the API, when called within detail::activation_poll_min_interval of this
    // client's previous poll of the same request, or before a Retry-After the
    // server sent has passed.
    //
    // A 404 also reads as pending. The API reads requests with eventual
    // consistency, so a fresh one can briefly answer 404, and Moonbase's 404 cannot
    // be told apart from one a proxy sent. A request nobody completes still ends:
    // the server expires it after an hour and answers 400.
    //
    // Throws activation_request_error when the server answers 400: the request
    // expired, was cancelled, or was refused, so stop polling and start a new one.
    // Transport failures, rate limiting and other server errors throw api_error
    // and are worth retrying.
    [[nodiscard]] std::optional<license> get_requested_activation(
        const activation_request& activation) const
    {
        if (!poll_gate_->try_claim(activation.request_url)) {
            return std::nullopt;
        }

        http_request request;
        request.method = "GET";
        request.url = activation.request_url;
        request.headers = detail::default_headers(options_);
        request.connect_timeout = options_.http_connect_timeout;
        request.request_timeout = options_.http_request_timeout;

        const auto response = transport_->send(request);
        if (response.status_code == 204 || response.status_code == 404) {
            return std::nullopt;
        }
        if (response.status_code < 200 || response.status_code >= 300) {
            if (const auto wait = detail::retry_after(response);
                wait && (response.status_code == 429 || response.status_code == 503)) {
                poll_gate_->back_off(*wait);
            }
            detail::throw_for_activation_poll_problem(response);
        }
        detail::ensure_token_response(response);
        return validator_->validate_token(response.body);
    }

private:
    licensing_options options_;
    std::shared_ptr<device_id_resolver> device_ids_;
    std::shared_ptr<license_validator> validator_;
    std::shared_ptr<http_transport> transport_;
    // Shared, so a copy of this client cannot double the poll rate.
    std::shared_ptr<detail::activation_poll_gate> poll_gate_ =
        std::make_shared<detail::activation_poll_gate>();
};

} // namespace moonbase
