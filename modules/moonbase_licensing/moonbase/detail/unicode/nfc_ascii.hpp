#pragma once

// NFC followed by a printable-ASCII filter, without ICU.
//
// THIS IS NOT A GENERAL NFC IMPLEMENTATION AND MUST NOT BE REUSED AS ONE. It
// answers exactly one question, and only because of what runs immediately after
// it: given a string, which printable-ASCII characters does NFC leave visible?
// It never materializes a normalized string, and it is only correct downstream
// of the U+0020..U+007E filter.
//
// The device fingerprint spec (FINGERPRINT_SPEC.md) requires NFC before that
// filter, and skipping it is not cosmetic: "cafe" + U+0301 must canonicalize to
// "caf", not "cafe", because NFC first composes the pair into a precomposed
// e-acute that the filter then drops whole. Getting this wrong yields a device
// id that disagrees with every other Moonbase SDK on the same machine.
//
// Because the filter discards everything outside printable ASCII, NFC can only
// change the visible result in three ways, and scripts/gen-nfc-tables.py
// enumerates all three exhaustively from the Unicode database:
//
//   1. An ASCII starter is consumed by a following combining mark.
//   2. A non-ASCII code point whose NFC form *is* a printable-ASCII character
//      (exactly three exist, all singleton decompositions).
//   3. A combining mark with its own canonical decomposition, expanded before
//      the blocking analysis in (1).
//
// Blocking collapses to a single question per cluster, because once an ASCII
// base composes the result is non-ASCII forever and no later composition can
// bring it back. Canonical ordering is a stable sort by combining class, so a
// mark is blocked exactly when an earlier mark in the cluster shares its class.
// The base is therefore annihilated if and only if it composes with some mark
// that is the first of its class in the cluster, which needs no sorting.
//
// scripts/gen-nfc-tables.py --verify differential-tests this against real NFC
// over every code point, every printable-ASCII base crossed with every combining
// mark, exhaustive base x mark x mark for a sample of bases, and several hundred
// thousand random strings.
//
// It is exact except in one deliberately bounded way. The shipped combining-class
// table covers the five combining-mark blocks rather than all of Unicode, because
// the full table costs 8.5 KB of header for nothing the spec exercises. A mark
// outside those blocks therefore reads as a starter, which ends the cluster, so a
// later composing mark never gets the chance to annihilate the base. Reaching that
// needs an ASCII base, then a combining mark from a non-Latin script, then a Latin
// composing mark. No IOKit UUID, machine-id, sysfs DMI string, SMBIOS string or
// host name contains such a sequence, and --verify asserts the shape of every
// divergence, that it always errs towards keeping the base, and that no input of
// one or two code points diverges at all.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "moonbase/detail/unicode/nfc_tables.hpp"

namespace moonbase::detail::unicode {

/// Lowest and highest characters the fingerprint material may contain.
inline constexpr char32_t printable_ascii_min = 0x20;
inline constexpr char32_t printable_ascii_max = 0x7E;

/// A byte that could not begin or continue a well-formed UTF-8 sequence.
///
/// Kept as an opaque non-ASCII starter rather than dropped outright: it ends any
/// combining cluster in progress but never annihilates a preceding base. That is
/// what Node produces for the same input, since its UTF-8 decoder substitutes
/// U+FFFD, which is likewise a non-ASCII starter. Firmware strings are the
/// realistic source of such bytes.
inline constexpr char32_t malformed_code_point = 0x7FFFFFFF;

namespace detail {

/// Canonical combining class, or 0 for a starter.
[[nodiscard]] inline std::uint8_t combining_class(char32_t code_point) noexcept
{
    const auto* ranges = tables::combining_class_ranges;
    std::size_t low = 0;
    std::size_t high = tables::combining_class_range_count;

    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (code_point < ranges[middle].first) {
            high = middle;
        } else if (code_point > ranges[middle].last) {
            low = middle + 1;
        } else {
            return ranges[middle].combining_class;
        }
    }

    return 0;
}

/// Bit position for a combining class that some composing mark uses, else -1.
[[nodiscard]] inline int combining_class_slot(std::uint8_t combining) noexcept
{
    for (std::size_t index = 0; index != tables::composing_mark_class_count; ++index) {
        if (tables::composing_mark_classes[index] == combining) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

/// Does `base` form a primary composite with `mark`?
[[nodiscard]] inline bool composes(char32_t base, char32_t mark) noexcept
{
    if (base < tables::composition_mask_first || base > tables::composition_mask_last) {
        return false;
    }
    if (mark < tables::composing_mark_first || mark > tables::composing_mark_last) {
        return false;
    }

    const auto bit = tables::composing_mark_index[mark - tables::composing_mark_first];
    if (bit < 0) {
        return false;
    }

    const auto mask = tables::composition_masks[base - tables::composition_mask_first];
    return (mask & (std::uint32_t{1} << bit)) != 0;
}

/// Apply the singleton decompositions that expose a printable-ASCII character.
[[nodiscard]] inline char32_t map_ascii_singleton(char32_t code_point) noexcept
{
    for (std::size_t index = 0; index != tables::ascii_singleton_count; ++index) {
        if (tables::ascii_singletons[index].from == code_point) {
            return tables::ascii_singletons[index].to;
        }
    }
    return code_point;
}

/// Canonical decomposition of a combining mark, or null when it has none.
[[nodiscard]] inline const tables::mark_decomposition* decompose_mark(char32_t code_point) noexcept
{
    for (std::size_t index = 0; index != tables::mark_decomposition_count; ++index) {
        if (tables::mark_decompositions[index].from == code_point) {
            return &tables::mark_decompositions[index];
        }
    }
    return nullptr;
}

/// Decode one UTF-8 sequence, advancing `offset`.
///
/// Strict: overlong encodings, surrogates, values above U+10FFFF and truncated
/// sequences all yield `malformed_code_point`. On failure exactly one byte is
/// consumed, so a following ASCII byte can never be swallowed by a bad prefix
/// ("A\xC3B" keeps both the A and the B).
[[nodiscard]] inline char32_t decode_utf8(std::string_view text, std::size_t& offset) noexcept
{
    const auto lead = static_cast<unsigned char>(text[offset]);

    if (lead < 0x80) {
        offset += 1;
        return lead;
    }

    std::size_t length = 0;
    char32_t code_point = 0;
    char32_t lowest = 0;

    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        code_point = lead & 0x1FU;
        lowest = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        code_point = lead & 0x0FU;
        lowest = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        code_point = lead & 0x07U;
        lowest = 0x10000;
    } else {
        // A continuation byte with no lead, or an invalid 5/6-byte prefix.
        offset += 1;
        return malformed_code_point;
    }

    if (offset + length > text.size()) {
        offset += 1;
        return malformed_code_point;
    }

    for (std::size_t index = 1; index != length; ++index) {
        const auto continuation = static_cast<unsigned char>(text[offset + index]);
        if ((continuation & 0xC0U) != 0x80U) {
            offset += 1;
            return malformed_code_point;
        }
        code_point = (code_point << 6U) | (continuation & 0x3FU);
    }

    // Overlong, surrogate, or beyond the Unicode range.
    if (code_point < lowest || (code_point >= 0xD800 && code_point <= 0xDFFF) || code_point > 0x10FFFF) {
        offset += 1;
        return malformed_code_point;
    }

    offset += length;
    return code_point;
}

} // namespace detail

/// NFC-normalize `text` and keep only the printable-ASCII characters that
/// survive, in order.
///
/// The result is pure ASCII, so its length in characters equals its length in
/// bytes. Truncation and trimming are the fingerprint spec's business and happen
/// in fingerprint_spec::canonicalize_value, not here.
[[nodiscard]] inline std::string canonicalize_printable_ascii(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    // The printable-ASCII starter whose cluster is still open, or 0 for none.
    char32_t pending = 0;
    bool annihilated = false;
    std::uint32_t seen_classes = 0;

    const auto flush = [&]() {
        if (pending != 0 && !annihilated) {
            out.push_back(static_cast<char>(pending));
        }
        pending = 0;
        annihilated = false;
        seen_classes = 0;
    };

    const auto consume = [&](char32_t code_point) {
        const std::uint8_t combining = detail::combining_class(code_point);

        if (combining == 0) {
            // A starter closes the previous cluster and opens its own.
            flush();
            const char32_t mapped = detail::map_ascii_singleton(code_point);
            if (mapped >= printable_ascii_min && mapped <= printable_ascii_max) {
                pending = mapped;
            }
            return;
        }

        // A combining mark. Marks are never printable ASCII, so the only thing
        // one can do is decide the fate of the base it is attached to.
        if (pending == 0 || annihilated) {
            return;
        }

        const int slot = detail::combining_class_slot(combining);
        if (slot < 0) {
            // No composing mark uses this class, so it can neither compose with
            // the base nor block a mark that would.
            return;
        }

        const std::uint32_t bit = std::uint32_t{1} << slot;
        if ((seen_classes & bit) != 0) {
            // Blocked by an earlier mark of the same class.
            return;
        }
        seen_classes |= bit;

        if (detail::composes(pending, code_point)) {
            annihilated = true;
        }
    };

    std::size_t offset = 0;
    while (offset < text.size()) {
        const char32_t code_point = detail::decode_utf8(text, offset);

        // A mark with its own canonical decomposition is expanded in place. Both
        // halves keep their original position, which is what a stable canonical
        // sort would do, so the blocking analysis stays correct.
        if (const auto* decomposition = detail::decompose_mark(code_point)) {
            for (std::uint8_t index = 0; index != decomposition->length; ++index) {
                consume(decomposition->to[index]);
            }
            continue;
        }

        consume(code_point);
    }

    flush();
    return out;
}

} // namespace moonbase::detail::unicode
