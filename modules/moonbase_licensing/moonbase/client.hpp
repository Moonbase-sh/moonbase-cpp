#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "moonbase/detail/url.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint.hpp"
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
    if (options.client_info && !options.client_info->empty()) {
        user_agent += " " + *options.client_info;
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

inline void throw_for_problem(long status_code, const std::string& body)
{
    std::string title;
    std::string detail;
    std::string error_type;

    if (!body.empty()) {
        try {
            const auto problem = nlohmann::json::parse(body);
            if (problem.contains("title") && problem.at("title").is_string()) {
                title = problem.at("title").get<std::string>();
            }
            if (problem.contains("detail") && problem.at("detail").is_string()) {
                detail = problem.at("detail").get<std::string>();
            }
            if (problem.contains("errorType")) {
                if (problem.at("errorType").is_string()) {
                    error_type = problem.at("errorType").get<std::string>();
                } else if (problem.at("errorType").is_number_integer()) {
                    error_type = std::to_string(problem.at("errorType").get<int>());
                }
            }
        } catch (const std::exception&) {
        }
    }

    const auto message = !detail.empty()
        ? detail
        : (!title.empty() ? title : "Moonbase API request failed with HTTP " + std::to_string(status_code));

    if (error_type == "LicenseExpired" || error_type == "4" ||
        title.find("expired") != std::string::npos ||
        detail.find("expired") != std::string::npos) {
        throw license_expired_error(message);
    }

    if (status_code == 400) {
        throw license_invalid_error(message);
    }

    throw api_error(static_cast<int>(status_code), message, title, detail);
}

} // namespace detail

class license_client {
public:
    license_client(
        licensing_options options,
        std::shared_ptr<fingerprint_provider> fingerprints,
        std::shared_ptr<license_validator> validator,
        std::shared_ptr<http_transport> transport)
        : options_(std::move(options)),
          fingerprints_(std::move(fingerprints)),
          validator_(std::move(validator)),
          transport_(std::move(transport))
    {
        if (!fingerprints_) {
            throw configuration_error("A fingerprint provider is required");
        }
        if (!validator_) {
            throw configuration_error("A license validator is required");
        }
        if (!transport_) {
            throw configuration_error("An HTTP transport is required");
        }
    }

    [[nodiscard]] activation_request request_activation() const
    {
        const auto url = detail::append_query(
            detail::request_path(options_),
            detail::client_query(options_));

        const auto payload = nlohmann::json{
            {"deviceName", fingerprints_->device_name()},
            {"deviceSignature", fingerprints_->device_id()},
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
            detail::throw_for_problem(response.status_code, response.body);
        }

        try {
            return nlohmann::json::parse(response.body).get<activation_request>();
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
            detail::throw_for_problem(response.status_code, response.body);
        }
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
            detail::throw_for_problem(response.status_code, response.body);
        }
    }

    [[nodiscard]] std::optional<license> get_requested_activation(
        const activation_request& activation) const
    {
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
            detail::throw_for_problem(response.status_code, response.body);
        }
        return validator_->validate_token(response.body);
    }

private:
    licensing_options options_;
    std::shared_ptr<fingerprint_provider> fingerprints_;
    std::shared_ptr<license_validator> validator_;
    std::shared_ptr<http_transport> transport_;
};

} // namespace moonbase
