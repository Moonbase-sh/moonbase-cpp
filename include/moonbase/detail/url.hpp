#pragma once

#include <cctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace moonbase::detail {

inline std::string trim_trailing_slashes(std::string value)
{
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

inline std::string url_encode(std::string_view value)
{
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

inline std::string append_query(
    const std::string& base_url,
    const std::map<std::string, std::string>& query)
{
    if (query.empty()) {
        return base_url;
    }

    std::ostringstream out;
    out << base_url << (base_url.find('?') == std::string::npos ? '?' : '&');
    bool first = true;
    for (const auto& [key, value] : query) {
        if (!first) {
            out << '&';
        }
        first = false;
        out << url_encode(key) << '=' << url_encode(value);
    }
    return out.str();
}

} // namespace moonbase::detail
