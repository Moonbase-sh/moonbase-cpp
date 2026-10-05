#pragma once

// UTF-16 to UTF-8, for the strings Windows only hands out faithfully through its
// wide APIs. The narrow ("A") variants answer in the ANSI code page instead, so a
// computer name such as "Björn-PC" comes back as bytes that are not UTF-8, and
// nlohmann::json refuses to serialize those.
//
// Plain C++ with no OS headers, so it is tested on every platform rather than
// only on the one it runs on.

#include <cstddef>
#include <string>
#include <string_view>

namespace moonbase::detail::unicode {

/// Transcode UTF-16 to UTF-8.
///
/// Never fails: an unpaired surrogate becomes U+FFFD, as WideCharToMultiByte
/// does, so the result is always valid UTF-8 and always safe to serialize.
[[nodiscard]] inline std::string utf16_to_utf8(std::u16string_view text)
{
    std::string out;
    out.reserve(text.size());

    for (std::size_t index = 0; index != text.size(); ++index) {
        char32_t code_point = text[index];

        const bool high_surrogate = code_point >= 0xD800 && code_point <= 0xDBFF;
        const bool low_surrogate = code_point >= 0xDC00 && code_point <= 0xDFFF;
        if (high_surrogate && index + 1 != text.size() && text[index + 1] >= 0xDC00
            && text[index + 1] <= 0xDFFF) {
            code_point = 0x10000 + ((code_point - 0xD800) << 10U) + (text[index + 1] - 0xDC00U);
            ++index;
        } else if (high_surrogate || low_surrogate) {
            code_point = 0xFFFD;
        }

        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }

    return out;
}

} // namespace moonbase::detail::unicode
