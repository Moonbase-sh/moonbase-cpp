#pragma once

// Licenses that cover only some versions of the app (license::release_access):
// how a version is read, whether a license covers it, and the error a license
// that doesn't is refused with. The rule is the one Moonbase and every other
// Moonbase SDK apply; tests/vectors/release-access-vectors.json pins it down case
// by case.

#include <chrono>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "moonbase/errors.hpp"
#include "moonbase/types.hpp"
#include "moonbase/version.hpp"

namespace moonbase {

// Reads the version an app says it is, the way Moonbase does when it judges
// whether a license covers it. Forgiving: an optional leading "v", then two to
// four numbers separated by dots (missing ones are 0, a fourth is ignored, so an
// assembly version works), with any "-pre-release", "+build" or space-separated
// suffix dropped, so "3.0.0-beta.2" is judged as 3.0.0. The result's prerelease
// is therefore always empty.
//
// Empty when the version can't be read, which means it is not checked at all.
// Stricter than parse_semver, which also takes a lone major ("3").
[[nodiscard]] inline std::optional<semver> parse_app_version(std::string_view version)
{
    // As in every other Moonbase SDK, so a version only the backend would read
    // never gets judged differently on the device.
    constexpr long long max_part = 9999999;

    std::size_t start = 0;
    std::size_t end = version.size();
    while (start < end && std::isspace(static_cast<unsigned char>(version[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(version[end - 1]))) {
        --end;
    }
    auto text = version.substr(start, end - start);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (const auto suffix = text.find_first_of("-+ "); suffix != std::string_view::npos) {
        text = text.substr(0, suffix);
    }

    const auto parts = detail::split(text, '.');
    if (parts.size() < 2 || parts.size() > 4) {
        return std::nullopt;
    }

    long long numbers[3] = {0, 0, 0};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (parts[index].empty()) {
            return std::nullopt;
        }
        long long number = 0;
        for (const char ch : parts[index]) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            number = number * 10 + (ch - '0');
            if (number > max_part) {
                return std::nullopt;
            }
        }
        if (index < 3) {
            numbers[index] = number;
        }
    }

    semver result;
    result.major = numbers[0];
    result.minor = numbers[1];
    result.patch = numbers[2];
    return result;
}

// Whether release access covers a version of the app: the same rule Moonbase
// applies when validating, and the one the validator checks application_version
// against.
//
// A version is covered when its major is at most up_to_major, or while
// updates_until has not passed, or, once it has, when it is at most
// up_to_version (every version, when nothing was published before updates
// ended). Without updates_until or up_to_major every version is covered. A
// version that can't be read (see parse_app_version) is not checked, so it
// counts as covered.
[[nodiscard]] inline bool covers_version(
    const license_release_access& access,
    std::string_view version,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    // As Moonbase reads a license token: without an end to updates or a highest
    // major, it covers every release.
    if (!access.updates_until && !access.up_to_major) {
        return true;
    }

    const auto app_version = parse_app_version(version);
    if (!app_version) {
        return true;
    }

    if (access.up_to_major && app_version->major <= *access.up_to_major) {
        return true;
    }

    if (!access.updates_until) {
        return false;
    }

    if (now <= *access.updates_until) {
        return true;
    }

    // With nothing published by the end of the updates there is no version to
    // judge by, and locking out someone who paid is worse than letting them run
    // whatever came after.
    const auto last_covered = parse_app_version(access.up_to_version.value_or(std::string()));
    return !last_covered || compare(*app_version, *last_covered) <= 0;
}

// Whether a license covers a version of the app. One without release access
// covers every version.
[[nodiscard]] inline bool covers_version(
    const license& value,
    std::string_view version,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    return !value.release_access || covers_version(*value.release_access, version, now);
}

// The license is valid, but doesn't cover the version of the app that runs: it
// covers only the releases published while its updates lasted, or versions up to
// some major. Moonbase refuses to validate it for application_version, and the
// validator refuses it on the device the same way unless enforce_release_access
// is off. Downloading a release the customer's licenses don't cover is refused
// with it too.
//
// Not a reason to delete the stored license: it still covers the versions it
// did, so the customer can go back to one of them, or upgrade and keep going.
// Say what it covers (release_access()) and offer the upgrade (upgrade_url()).
//
// Derives from license_invalid_error, as license_device_mismatch_error does, so
// code that refuses to run on an invalid license refuses here too, and the
// offline grace period can't keep a version running that the license doesn't
// cover. Catch it before license_invalid_error to keep the license.
class release_not_covered_error : public license_invalid_error {
public:
    release_not_covered_error(
        const std::string& message,
        std::optional<moonbase::license> refused,
        std::string app_version,
        std::optional<license_release_access> access = std::nullopt,
        std::optional<std::string> upgrade_url = std::nullopt)
        : license_invalid_error(error_type::release_not_covered, message),
          license_(std::move(refused)),
          app_version_(std::move(app_version)),
          release_access_(std::move(access)),
          upgrade_url_(std::move(upgrade_url))
    {
        if (!release_access_ && license_) {
            release_access_ = license_->release_access;
        }
    }

    // The license, still good for the versions it covers. When Moonbase refused
    // it on validation, the copy it issued along with the refusal: store it in
    // place of the old one. Empty when a download was refused.
    [[nodiscard]] const std::optional<moonbase::license>& license() const noexcept { return license_; }

    // The version that isn't covered, as MAJOR.MINOR.PATCH: the app's, as
    // Moonbase read it. Empty when a download was refused, which names none.
    [[nodiscard]] const std::string& app_version() const noexcept { return app_version_; }

    // What the license covers: as Moonbase described it when it refused, else
    // what the license says.
    [[nodiscard]] const std::optional<license_release_access>& release_access() const noexcept
    {
        return release_access_;
    }

    // Where the customer buys the upgrade or renewal. Only Moonbase knows it, so
    // it is empty when the device refused, and when the store sells none.
    [[nodiscard]] const std::optional<std::string>& upgrade_url() const noexcept { return upgrade_url_; }

private:
    std::optional<moonbase::license> license_;
    std::string app_version_;
    std::optional<license_release_access> release_access_;
    std::optional<std::string> upgrade_url_;
};

} // namespace moonbase
