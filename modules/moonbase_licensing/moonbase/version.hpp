#pragma once

// Minimal Semantic Versioning compare, just enough to decide whether the
// backend's "currently released version" (the license's p:rel claim) is newer
// than the running app's version. Header-only and dependency-free so it can be
// unit-tested in isolation.

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moonbase {

struct semver {
    long long major = 0;
    long long minor = 0;
    long long patch = 0;
    std::string prerelease; // dot-joined identifiers; empty for a normal release
};

namespace detail {

// Whole-string unsigned integer (no sign, no overflow guard beyond long long;
// version numbers are tiny). Returns nullopt for an empty / non-digit run.
inline std::optional<long long> parse_uint(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    long long value = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

inline std::vector<std::string_view> split(std::string_view text, char delim)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto pos = text.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

} // namespace detail

// Tolerant parse: accepts a leading "v"/"V", a missing minor/patch ("2.4" ->
// 2.4.0), a "-prerelease" suffix and a "+build" suffix (build metadata is
// ignored per SemVer). Returns nullopt when the numeric core is malformed.
inline std::optional<semver> parse_semver(std::string_view input)
{
    std::size_t i = 0;
    while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) {
        ++i;
    }
    std::size_t end = input.size();
    while (end > i && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    auto text = input.substr(i, end - i);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    // Strip build metadata first (+...), then split the prerelease (-...).
    if (const auto plus = text.find('+'); plus != std::string_view::npos) {
        text = text.substr(0, plus);
    }
    std::string_view core = text;
    std::string_view pre;
    if (const auto dash = text.find('-'); dash != std::string_view::npos) {
        core = text.substr(0, dash);
        pre = text.substr(dash + 1);
    }

    const auto parts = detail::split(core, '.');
    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }

    semver result;
    const auto major = detail::parse_uint(parts[0]);
    if (!major) {
        return std::nullopt;
    }
    result.major = *major;
    if (parts.size() > 1) {
        const auto minor = detail::parse_uint(parts[1]);
        if (!minor) {
            return std::nullopt;
        }
        result.minor = *minor;
    }
    if (parts.size() > 2) {
        const auto patch = detail::parse_uint(parts[2]);
        if (!patch) {
            return std::nullopt;
        }
        result.patch = *patch;
    }
    result.prerelease = std::string(pre);
    return result;
}

// SemVer §11 precedence: -1 if a < b, 0 if equal, 1 if a > b.
inline int compare(const semver& a, const semver& b)
{
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }

    // A release outranks any prerelease of the same core version.
    if (a.prerelease.empty() != b.prerelease.empty()) {
        return a.prerelease.empty() ? 1 : -1;
    }
    if (a.prerelease.empty()) {
        return 0;
    }

    const auto lhs = detail::split(a.prerelease, '.');
    const auto rhs = detail::split(b.prerelease, '.');
    const auto count = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto ln = detail::parse_uint(lhs[i]);
        const auto rn = detail::parse_uint(rhs[i]);
        if (ln && rn) {
            if (*ln != *rn) {
                return *ln < *rn ? -1 : 1;
            }
        } else if (ln) {
            return -1; // numeric identifiers have lower precedence than alphanumeric
        } else if (rn) {
            return 1;
        } else if (lhs[i] != rhs[i]) {
            return lhs[i] < rhs[i] ? -1 : 1;
        }
    }
    if (lhs.size() != rhs.size()) {
        return lhs.size() < rhs.size() ? -1 : 1; // more identifiers wins
    }
    return 0;
}

// True when `latest` is a strictly newer version than `current`. Returns false
// when either string fails to parse, so a malformed value never prompts an
// update.
inline bool update_available(std::string_view current, std::string_view latest)
{
    const auto a = parse_semver(current);
    const auto b = parse_semver(latest);
    if (!a || !b) {
        return false;
    }
    return compare(*b, *a) > 0;
}

} // namespace moonbase
