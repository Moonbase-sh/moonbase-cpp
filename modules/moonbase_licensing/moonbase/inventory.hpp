#pragma once

// Client for the backend "inventory" endpoints (/api/customer/inventory/...),
// used to fetch a release's notes and an authenticated installer download URL
// for the running platform. Authenticates with the license token via the
// custom "Authorization: LicenseToken <jwt>" scheme. Mirrors license_client and
// reuses its request helpers (detail::append_query / default_headers /
// throw_for_inventory_problem).

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
#include "moonbase/release_access.hpp"
#include "moonbase/types.hpp"

namespace moonbase {

// The plain-text release notes plus enough to know an installer exists and
// whether this customer may download it.
struct release_info {
    std::string version;     // the queried release version (echoed back by the server)
    std::string description; // release notes (plain text); empty when none
    bool has_downloads = false;
    bool downloads_need_user = false;      // download requires an authenticated customer
    bool downloads_need_ownership = false; // download requires owning / subscribing to the product

    // Whether the customer's licenses cover this release. False only when
    // Moonbase says they don't, for a license that covers only some versions;
    // downloading it is then refused with release_not_covered_error.
    bool release_covered = true;
    // What the customer's licenses cover, when that isn't every release.
    std::optional<license_release_access> release_access;
    // Where to buy the upgrade or renewal, while the store sells one.
    std::optional<std::string> upgrade_url;

    // Fields are only ever appended, so a brace-initialized release_info keeps
    // its meaning.
    bool downloads_need_group_membership = false; // download requires belonging to a group of customers
    // Whether the customer the license token belongs to may download this
    // release, as Moonbase decides it with every rule the release has
    // (ownership, customer groups, the versions their licenses cover). Empty
    // from an older Moonbase that doesn't send it.
    std::optional<bool> downloads_allowed;
};

// A resolved, ready-to-fetch installer URL. `url` is a short-lived presigned
// link (the backend expires it after ~15 minutes). `filename` is a best-effort
// suggestion parsed from the URL; callers should fall back to their own name
// when it is empty.
struct download_target {
    std::string url;
    std::string filename;
};

namespace detail {

// An authenticated customer has a real user id, while an anonymous trial
// carries the nil GUID.
inline bool has_signed_in_customer(const license& lic)
{
    return !lic.issued_to.id.empty() && lic.issued_to.id != "00000000-0000-0000-0000-000000000000";
}

} // namespace detail

// Whether `license` may download the installer of a release (from
// get_release). Moonbase says so itself (downloads_allowed). Without that, from
// an older Moonbase, it is worked out as Moonbase would for the common rules: a
// full (non-trial) license owns or subscribes to the product and a trial does
// not, a signed-in customer has a user id where an anonymous trial has none,
// and an owner gets only a release their licenses cover.
inline bool can_download(const license& lic, const release_info& info)
{
    if (!info.release_covered) {
        return false;
    }
    if (info.downloads_allowed) {
        return *info.downloads_allowed;
    }
    if (info.downloads_need_ownership && lic.trial) {
        return false;
    }
    if (info.downloads_need_user && !detail::has_signed_in_customer(lic)) {
        return false;
    }
    return true;
}

// Whether a full license is what keeps `license` from downloading a release its
// licenses cover: the release is for owners and this is a trial, or for
// signed-in customers and this license has none. Activating a full license,
// which signs the customer in too, is then the way to it. False when it can
// download, and when Moonbase may refuse it for something a full license
// doesn't change: a release limited to a group of customers, which buying
// doesn't join, or to no one.
inline bool download_needs_full_license(const license& lic, const release_info& info)
{
    if (!info.release_covered || info.downloads_need_group_membership || can_download(lic, info)) {
        return false;
    }
    return (info.downloads_need_ownership && lic.trial)
        || (info.downloads_need_user && !detail::has_signed_in_customer(lic));
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
    //
    // Every failure throws api_error with the HTTP status. These endpoints never
    // refuse the license itself: a token they cannot use just makes the request
    // anonymous, so a private product then answers 404. The result says
    // whether that customer may download the release (downloads_allowed), and,
    // for a customer whose licenses don't cover every release, whether they
    // cover this one (release_covered) and where to upgrade.
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
            detail::throw_for_inventory_problem(response);
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
            if (json.contains("downloadsNeedsGroupMembership") && json.at("downloadsNeedsGroupMembership").is_boolean()) {
                info.downloads_need_group_membership = json.at("downloadsNeedsGroupMembership").get<bool>();
            }
            if (json.contains("downloadsAllowed") && json.at("downloadsAllowed").is_boolean()) {
                info.downloads_allowed = json.at("downloadsAllowed").get<bool>();
            }
            // Present only when the customer's licenses don't cover every release.
            if (json.contains("releaseAccess") && json.at("releaseAccess").is_object()) {
                const auto& access = json.at("releaseAccess");
                if (access.contains("coversRelease") && access.at("coversRelease").is_boolean()) {
                    info.release_covered = access.at("coversRelease").get<bool>();
                }
                info.release_access = detail::release_access_from(json);
                info.upgrade_url = detail::problem_string(access, "upgradeUrl");
            }
            return info;
        } catch (const std::exception& ex) {
            throw api_error(static_cast<int>(response.status_code),
                            std::string("Could not parse product response: ") + ex.what());
        }
    }

    // GET /api/customer/inventory/products/{id}/download/{platform}/latest?redirect=false
    // resolves the latest installer for the platform to a presigned URL.
    // `platform_name` is the backend Platform enum name (e.g. "Mac", "Windows").
    // Throws api_error: 404 when no installer exists for the platform or the
    // product has no release, 403 when this license may not download it, and
    // release_not_covered_error, with the upgrade link, when the customer's
    // licenses don't cover the release.
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
            detail::throw_for_inventory_problem(response);
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
