#pragma once

// Client for the backend "inventory" endpoints (/api/customer/inventory/...),
// used to fetch a release's notes and an authenticated installer download URL
// for the running platform. Authenticates with the license token via the
// custom "Authorization: LicenseToken <jwt>" scheme. Mirrors license_client and
// reuses its request helpers (detail::append_query / default_headers /
// throw_for_problem).

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/detail/url.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/http.hpp"
#include "moonbase/types.hpp"

namespace moonbase {

// The plain-text release notes plus enough to know an installer exists and who
// may download it (the product's release access-control level).
struct release_info {
    std::string version;     // the queried release version (echoed back by the server)
    std::string description; // release notes (plain text); empty when none
    bool has_downloads = false;
    bool downloads_need_user = false;      // download requires an authenticated customer
    bool downloads_need_ownership = false; // download requires owning / subscribing to the product
};

// A resolved, ready-to-fetch installer URL. `url` is a short-lived presigned
// link (the backend expires it after ~15 minutes). `filename` is a best-effort
// suggestion parsed from the URL; callers should fall back to their own name
// when it is empty.
struct download_target {
    std::string url;
    std::string filename;
};

// Whether `license` may download the installer for a release with the given
// access flags (from release_info). Mirrors the backend's release access control:
// a full (non-trial) license owns or subscribes to the product, a trial does not;
// an authenticated customer has a real user id, while an anonymous trial carries
// the nil GUID.
inline bool can_download(const license& lic, const release_info& info)
{
    const bool owns = !lic.trial;
    const bool has_user = !lic.issued_to.id.empty()
        && lic.issued_to.id != "00000000-0000-0000-0000-000000000000";
    if (info.downloads_need_ownership && !owns) {
        return false;
    }
    if (info.downloads_need_user && !has_user) {
        return false;
    }
    return true;
}

namespace detail {

inline std::string inventory_product_path(const licensing_options& options)
{
    return trim_trailing_slashes(options.endpoint) +
        "/api/customer/inventory/products/" +
        url_encode(options.product_id);
}

inline std::string inventory_latest_download_path(const licensing_options& options,
                                                  const std::string& platform_name)
{
    return inventory_product_path(options) + "/download/" + url_encode(platform_name) + "/latest";
}

// Decode percent-escapes (and '+' as space) so a filename embedded in a
// content-disposition query parameter is readable.
inline std::string url_decode(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '%' && i + 2 < value.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(value[i + 1]);
            const int lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(ch == '+' ? ' ' : ch);
    }
    return out;
}

// Best-effort installer filename from a presigned URL's content-disposition
// (S3 puts it in the response-content-disposition query param). Returns "" when
// not found.
inline std::string filename_from_download_url(const std::string& url)
{
    const auto decoded = url_decode(url);
    const auto at = decoded.find("filename");
    if (at == std::string::npos) {
        return {};
    }
    auto rest = std::string_view(decoded).substr(at + 8); // past "filename"
    if (!rest.empty() && rest.front() == '*') {
        rest.remove_prefix(1); // RFC 5987 "filename*="
    }
    const auto eq = rest.find('=');
    if (eq == std::string_view::npos) {
        return {};
    }
    rest.remove_prefix(eq + 1);
    // Drop an RFC 5987 charset prefix like "UTF-8''".
    if (const auto quote = rest.find("''"); quote != std::string_view::npos && quote < 12) {
        rest.remove_prefix(quote + 2);
    }
    std::string name;
    bool quoted = false;
    for (const char ch : rest) {
        if (ch == '"') {
            if (quoted) break;
            quoted = true;
            continue;
        }
        if (!quoted && (ch == ';' || ch == '&' || ch == ' ')) {
            break;
        }
        name.push_back(ch);
    }
    return name;
}

inline std::map<std::string, std::string> inventory_headers(const licensing_options& options,
                                                            std::string_view license_token)
{
    auto headers = default_headers(options);
    if (!license_token.empty()) {
        headers["Authorization"] = "LicenseToken " + std::string(license_token);
    }
    return headers;
}

} // namespace detail

class inventory_client {
public:
    inventory_client(licensing_options options, std::shared_ptr<http_transport> transport)
        : options_(std::move(options)), transport_(std::move(transport))
    {
        if (!transport_) {
            throw configuration_error("An HTTP transport is required");
        }
    }

    // GET /api/customer/inventory/products/{id}?version=<version>: release notes
    // for the given version (empty version asks for the current release).
    [[nodiscard]] release_info get_release(std::string_view version,
                                           std::string_view license_token) const
    {
        std::map<std::string, std::string> query{{"includeManifests", "false"}};
        if (!version.empty()) {
            query["version"] = std::string(version);
        }
        const auto url = detail::append_query(detail::inventory_product_path(options_), query);

        http_request request;
        request.method = "GET";
        request.url = url;
        request.headers = detail::inventory_headers(options_, license_token);
        request.connect_timeout = options_.http_connect_timeout;
        request.request_timeout = options_.http_request_timeout;

        const auto response = transport_->send(request);
        if (response.status_code < 200 || response.status_code >= 300) {
            detail::throw_for_problem(response.status_code, response.body);
        }

        try {
            const auto json = nlohmann::json::parse(response.body);
            release_info info;
            if (json.contains("version") && json.at("version").is_string()) {
                info.version = json.at("version").get<std::string>();
            } else if (json.contains("currentVersion") && json.at("currentVersion").is_string()) {
                info.version = json.at("currentVersion").get<std::string>();
            }
            if (json.contains("releaseDescription") && json.at("releaseDescription").is_string()) {
                info.description = json.at("releaseDescription").get<std::string>();
            }
            info.has_downloads = json.contains("downloads") && json.at("downloads").is_array()
                && !json.at("downloads").empty();
            info.downloads_need_user = json.value("downloadsNeedsUser", false);
            info.downloads_need_ownership = json.value("downloadsNeedsOwnership", false);
            return info;
        } catch (const std::exception& ex) {
            throw api_error(static_cast<int>(response.status_code),
                            std::string("Could not parse product response: ") + ex.what());
        }
    }

    // GET /api/customer/inventory/products/{id}/download/{platform}/latest?redirect=false
    // resolves the latest installer for the platform to a presigned URL.
    // `platform_name` is the backend Platform enum name (e.g. "Mac", "Windows").
    // Throws (404) when no installer exists for the platform.
    [[nodiscard]] download_target get_download_url(std::string_view platform_name,
                                                   std::string_view license_token) const
    {
        const auto url = detail::append_query(
            detail::inventory_latest_download_path(options_, std::string(platform_name)),
            {{"redirect", "false"}});

        http_request request;
        request.method = "GET";
        request.url = url;
        request.headers = detail::inventory_headers(options_, license_token);
        request.connect_timeout = options_.http_connect_timeout;
        request.request_timeout = options_.http_request_timeout;

        const auto response = transport_->send(request);
        if (response.status_code < 200 || response.status_code >= 300) {
            detail::throw_for_problem(response.status_code, response.body);
        }

        try {
            const auto json = nlohmann::json::parse(response.body);
            download_target target;
            target.url = json.at("location").get<std::string>();
            target.filename = detail::filename_from_download_url(target.url);
            return target;
        } catch (const std::exception& ex) {
            throw api_error(static_cast<int>(response.status_code),
                            std::string("Could not parse download response: ") + ex.what());
        }
    }

private:
    licensing_options options_;
    std::shared_ptr<http_transport> transport_;
};

} // namespace moonbase
