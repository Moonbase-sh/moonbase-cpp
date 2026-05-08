#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace moonbase::detail {

inline std::string base64_encode(const unsigned char* data, std::size_t length)
{
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((length + 2) / 3) * 4);

    for (std::size_t i = 0; i < length; i += 3) {
        const auto a = static_cast<std::uint32_t>(data[i]);
        const auto b = i + 1 < length ? static_cast<std::uint32_t>(data[i + 1]) : 0U;
        const auto c = i + 2 < length ? static_cast<std::uint32_t>(data[i + 2]) : 0U;
        const auto triple = (a << 16U) | (b << 8U) | c;

        out.push_back(table[(triple >> 18U) & 0x3FU]);
        out.push_back(table[(triple >> 12U) & 0x3FU]);
        out.push_back(i + 1 < length ? table[(triple >> 6U) & 0x3FU] : '=');
        out.push_back(i + 2 < length ? table[triple & 0x3FU] : '=');
    }

    return out;
}

inline std::string base64_encode(const std::vector<unsigned char>& data)
{
    return base64_encode(data.data(), data.size());
}

inline std::vector<unsigned char> base64_decode(std::string_view input)
{
    static constexpr unsigned char invalid = 0xFFU;
    static const auto reverse_table = [] {
        std::array<unsigned char, 256> table{};
        table.fill(invalid);
        const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            table[static_cast<unsigned char>(alphabet[i])] = static_cast<unsigned char>(i);
        }
        return table;
    }();

    std::string normalized;
    normalized.reserve(input.size());
    for (const char c : input) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            normalized.push_back(c);
        }
    }

    if (normalized.empty()) {
        return {};
    }
    if (normalized.size() % 4 != 0) {
        throw std::runtime_error("Invalid base64 length");
    }

    std::vector<unsigned char> out;
    out.reserve((normalized.size() / 4) * 3);

    for (std::size_t i = 0; i < normalized.size(); i += 4) {
        unsigned char values[4]{};
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const auto c = normalized[i + static_cast<std::size_t>(j)];
            if (c == '=') {
                values[j] = 0;
                ++padding;
                continue;
            }
            const auto v = reverse_table[static_cast<unsigned char>(c)];
            if (v == invalid) {
                throw std::runtime_error("Invalid base64 character");
            }
            values[j] = v;
        }

        const auto triple =
            (static_cast<std::uint32_t>(values[0]) << 18U) |
            (static_cast<std::uint32_t>(values[1]) << 12U) |
            (static_cast<std::uint32_t>(values[2]) << 6U) |
            static_cast<std::uint32_t>(values[3]);

        out.push_back(static_cast<unsigned char>((triple >> 16U) & 0xFFU));
        if (padding < 2) {
            out.push_back(static_cast<unsigned char>((triple >> 8U) & 0xFFU));
        }
        if (padding < 1) {
            out.push_back(static_cast<unsigned char>(triple & 0xFFU));
        }
    }

    return out;
}

inline std::string base64url_encode(const unsigned char* data, std::size_t length)
{
    auto out = base64_encode(data, length);
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

inline std::string base64url_encode(const std::vector<unsigned char>& data)
{
    return base64url_encode(data.data(), data.size());
}

inline std::vector<unsigned char> base64url_decode(std::string_view input)
{
    std::string normalized(input);
    std::replace(normalized.begin(), normalized.end(), '-', '+');
    std::replace(normalized.begin(), normalized.end(), '_', '/');
    while (normalized.size() % 4 != 0) {
        normalized.push_back('=');
    }
    return base64_decode(normalized);
}

inline std::string bytes_to_string(const std::vector<unsigned char>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace moonbase::detail
