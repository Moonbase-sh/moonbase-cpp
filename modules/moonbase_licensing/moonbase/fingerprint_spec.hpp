#pragma once

// The Moonbase device fingerprint specification, version 2.
//
// See FINGERPRINT_SPEC.md at the repo root for the normative text and
// tests/vectors/fingerprint-vectors.json for the conformance suite. When the
// spec prose and the vectors disagree, the vectors win: they are what every SDK
// can actually execute.
//
// A license token carries a `sig` claim equal to the device id, and each SDK
// recomputes that id locally and compares it on every offline validation. If two
// SDKs compute it differently on the same machine, a license activated by one
// will not validate in the other. This header is byte-exact and deterministic so
// they cannot: given the same platform tag and parameters, it produces the same
// device id as @moonbase.sh/licensing does.
//
// Everything here is pure. There are no OS headers and nothing reads the
// machine, so the whole file compiles and is tested on every platform. That is
// deliberate: it means the Windows SMBIOS parser and the macOS ioreg parser are
// exercised by CI on Linux and macOS runners too, rather than only where they
// happen to run. The platform reads themselves live in
// moonbase_device_id_resolver.hpp.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
// Macros only, no OS surface: this is how Apple platforms are told apart, and
// mac and ios are separate platform tags with different identity parameters.
#include <TargetConditionals.h>
#endif

#include "moonbase/detail/crypto/crypto.hpp"
#include "moonbase/detail/unicode/nfc_ascii.hpp"
#include "moonbase/errors.hpp"

namespace moonbase::fingerprint_spec {

/// First line of the hashed material. Always carries the same version number as
/// the device id stamp.
inline constexpr std::string_view prefix = "moonbase:fingerprint:v2";

/// Spec version, present in both the material prefix and the id stamp.
inline constexpr int version = 2;

/// Longest permitted canonical value, in characters.
inline constexpr std::size_t max_value_length = 128;

/// Where a device id's identity came from, and therefore what it may be compared
/// to. Each non-hardware source carries its own stamp so the limitation travels
/// with the value.
enum class device_id_source {
    /// Hardware identity. Comparable across every conforming SDK. `mbd2_`
    identity,
    /// The opt-in host-name fallback. `mbd2n_`
    device_name,
    /// Scoped identity: stable for this device within one *scope*, and not
    /// comparable across scopes or SDKs. The scope is the platform's, not ours:
    /// iOS scopes identifierForVendor to the App Store vendor (else the bundle id
    /// minus its last component), Android scopes ANDROID_ID to the app signing
    /// key. So it is narrower than "publisher": one vendor's two differently
    /// signed Android apps do not share an id. `mbd2s_`
    scoped,
};

using parameter = std::pair<std::string, std::string>;
using parameter_list = std::vector<parameter>;

struct device_id_stamp {
    /// Fingerprint spec version that produced the digest.
    int version = 0;
    /// The literal source tag: "", "n", "s", or one a newer SDK introduced.
    std::string source_tag;
    /// What source_tag means, or nothing when this SDK does not define that tag.
    std::optional<device_id_source> source;
    /// The 64-character lowercase-hex SHA-256.
    std::string digest;
};

namespace internal {

// Exactly these parameters describe the individual machine. Everything else a
// platform collects is model-level: vendor, product and board names are
// byte-identical across every unit of a product line, so a material built only
// from those would give every machine of that model the same device id, and each
// would validate the others' licenses.
inline constexpr std::array<std::string_view, 7> identifying_params{
    "ioPlatformUuid",
    "machineId",
    "systemUuid",
    "baseboardSerialNumber",
    // Scoped sources: identifying within the platform's own scope, which is all
    // these platforms allow. See the spec's "Scoped identity" section.
    "identifierForVendor",
    "androidId",
    "deviceName",
};

// OEM filler that is not really a value. Compared case-insensitively against the
// canonical value, and only for identifying parameters: a descriptive field
// reading "Default string" is still a fair description of the model, whereas a
// serial number reading it is not a serial number.
inline constexpr std::array<std::string_view, 15> not_programmed_values{
    "to be filled by o.e.m.",
    "to be filled by oem",
    "default string",
    "system serial number",
    "base board serial number",
    "chassis serial number",
    "not specified",
    "not applicable",
    "not available",
    "none",
    "unknown",
    "invalid",
    "n/a",
    "0123456789",
    "uninitialized",
};

[[nodiscard]] inline char to_lower(char value) noexcept
{
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] inline bool equals_ignoring_ascii_case(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index != left.size(); ++index) {
        if (to_lower(left[index]) != to_lower(right[index])) {
            return false;
        }
    }
    return true;
}

// The tags this version defines. The grammar a parser *accepts* is deliberately
// wider (see parse_device_id_stamp): a tag introduced by a newer SDK must still
// parse, or a perfectly valid id gets reported as "not a Moonbase device id".
//
// One table, consulted in both directions, so the stamper and the parser cannot
// drift apart.
struct source_tag_entry {
    device_id_source source;
    std::string_view tag;
};

inline constexpr std::array<source_tag_entry, 3> source_tags{{
    {device_id_source::identity, ""},
    {device_id_source::device_name, "n"},
    {device_id_source::scoped, "s"},
}};

[[nodiscard]] inline std::string_view tag_for_source(device_id_source source) noexcept
{
    for (const auto& entry : source_tags) {
        if (entry.source == source) {
            return entry.tag;
        }
    }
    return {};
}

[[nodiscard]] inline std::optional<device_id_source> source_for_tag(std::string_view tag) noexcept
{
    for (const auto& entry : source_tags) {
        if (entry.tag == tag) {
            return entry.source;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool is_lowercase_hex(std::string_view value) noexcept
{
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

} // namespace internal

/// Canonicalize a raw value: NFC, then printable ASCII only, then capped at 128
/// characters, then space-trimmed at both ends. In that order.
///
/// Interior spaces are preserved and nothing else is altered: no case folding,
/// no reordering. Truncation runs before trimming, so a value whose 128-character
/// prefix ends in spaces does not keep them.
///
/// Dropping everything outside U+0020..U+007E does more work than it looks. It
/// makes the material grammar unambiguous, since a value can no longer contain an
/// LF and so cannot forge an extra `name=value` line. It makes the decoding of
/// raw firmware strings very nearly irrelevant, since every byte two decoders
/// would disagree about is discarded. And it absorbs the trailing newline that
/// sysfs reads carry.
[[nodiscard]] inline std::string canonicalize_value(std::string_view value)
{
    auto printable = moonbase::detail::unicode::canonicalize_printable_ascii(value);

    // Pure ASCII by now, so characters and bytes are the same thing.
    if (printable.size() > max_value_length) {
        printable.resize(max_value_length);
    }

    const auto first = printable.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const auto last = printable.find_last_not_of(' ');
    return printable.substr(first, last - first + 1);
}

/// The platform tag for the host this was compiled for.
[[nodiscard]] inline std::string_view platform_tag() noexcept
{
    // Android must be tested before Linux: it defines both.
#if defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    // macOS and iOS are separate tags: they have entirely different identity
    // parameters, and an iOS id is scoped where a macOS one is not, so conflating
    // them would let two incomparable ids claim the same provenance. Every Apple
    // platform other than macOS maps to `ios`, because they all offer the same
    // single identifier and nothing else.
    //
    // Mac Catalyst counts as macOS. The spec states the rule as a runtime pair,
    // because Swift's compile-time tests get it wrong, but in C++ the compile-time
    // macros reproduce that table exactly:
    //
    //   isMacCatalystApp  isiOSAppOnMac  running as              tag   macro state
    //   false             false          a real iPhone / iPad    ios   MACCATALYST 0
    //   true              false          Mac Catalyst            mac   MACCATALYST 1
    //   true              true           iOS app on Apple silicon ios  MACCATALYST 0
    //
    // A Catalyst binary is always the middle row and an iOS binary is never it, so
    // TARGET_OS_MACCATALYST decides it without a runtime query. Note
    // TARGET_OS_IPHONE is 1 for Catalyst too, which is why it cannot be the test.
    //
    // This matters because a Catalyst app can read *both* identifierForVendor and
    // IOKit, so without a rule two SDKs on one Mac would disagree about which to
    // use. Hardware identity wins, and the Catalyst app then agrees with an
    // Electron or web SDK on the same machine.
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
    return "mac";
#else
    return "ios";
#endif
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    return "bsd";
#else
    return "unknown";
#endif
}

/// The identifying parameter names, in spec order.
///
/// Returned by value so a consumer cannot widen the set the material check
/// accepts. Handing out a reference to the table would let one line of caller
/// code turn "this machine has no identity" into "this model has an identity".
[[nodiscard]] inline std::vector<std::string_view> identifying_param_names()
{
    return {internal::identifying_params.begin(), internal::identifying_params.end()};
}

[[nodiscard]] inline bool is_identifying_param(std::string_view name) noexcept
{
    return std::find(internal::identifying_params.begin(), internal::identifying_params.end(), name)
        != internal::identifying_params.end();
}

/// Is this canonical value OEM filler rather than a real identifier?
///
/// An identifying value that is really filler is treated as absent, for the same
/// reason an all-FF SMBIOS UUID is: it is a constant shared by the whole product
/// line, so hashing it would hand every unit the same device id.
[[nodiscard]] inline bool is_not_programmed(std::string_view canonical_value) noexcept
{
    if (canonical_value.empty()) {
        return false;
    }

    // A blank UUID field or a zeroed machine-id.
    const bool all_zeroes = std::all_of(
        canonical_value.begin(), canonical_value.end(), [](char character) { return character == '0'; });
    const bool all_f = std::all_of(canonical_value.begin(), canonical_value.end(), [](char character) {
        return character == 'f' || character == 'F';
    });
    if (all_zeroes || all_f) {
        return true;
    }

    return std::any_of(
        internal::not_programmed_values.begin(),
        internal::not_programmed_values.end(),
        [canonical_value](std::string_view filler) {
            return internal::equals_ignoring_ascii_case(canonical_value, filler);
        });
}

/// Constants shared across devices that are rejected for `androidId` only.
///
/// Keyed by parameter on purpose. The shared placeholder list applies to every
/// identifying parameter, so putting this there would change the device id of a
/// machine that happens to report the same string as, say, a baseboard serial,
/// which the spec forbids without a version bump. A value that is meaningless as
/// an androidId can be a perfectly good Windows serial.
namespace internal {

inline constexpr std::array<std::string_view, 1> rejected_android_ids{
    // A real ANDROID_ID shared by a large batch of 2010-era devices whose
    // ro.serialno was unset, seeding the generator identically on every unit.
    // Valid hex, so the format rule cannot catch it.
    "9774d56d682e549c",
};

} // namespace internal

/// Does this canonical value look like a real Android SSAID?
///
/// `^[0-9a-f]{1,16}$` plus the placeholder rule. That regex is what makes the
/// classic mistake *mechanically* impossible rather than merely documented:
/// reading the `Settings.Secure.ANDROID_ID` static field yields the key name
/// "android_id", which is identical on every device and is not hex, so it can
/// never reach the material. JUCE's SystemStats::getUniqueDeviceID() has exactly
/// that defect.
///
/// The bound is 1..16 rather than exactly 16 because AOSP before 8.0 generated the
/// value with Long.toHexString, which drops leading zeros; requiring 16 would
/// reject legitimate ids on roughly one in sixteen pre-Oreo devices.
[[nodiscard]] inline bool is_valid_android_id(std::string_view canonical_value) noexcept
{
    if (canonical_value.empty() || canonical_value.size() > 16
        || !internal::is_lowercase_hex(canonical_value) || is_not_programmed(canonical_value)) {
        return false;
    }

    return std::none_of(
        internal::rejected_android_ids.begin(),
        internal::rejected_android_ids.end(),
        [canonical_value](std::string_view rejected) {
            return internal::equals_ignoring_ascii_case(canonical_value, rejected);
        });
}

/// Can this identifying value stand for the machine?
///
/// Not an unprogrammed placeholder, and matching the format the spec pins for that
/// parameter where it pins one. Only `androidId` has a pinned format, deliberately:
/// the rule exists to make one specific, currently-shipping defect impossible
/// rather than merely documented, and every extra rule is a new way to reject a
/// value some real device legitimately reports.
[[nodiscard]] inline bool is_usable_identity(std::string_view name, std::string_view canonical_value) noexcept
{
    if (is_not_programmed(canonical_value)) {
        return false;
    }
    if (name == "androidId") {
        return is_valid_android_id(canonical_value);
    }
    return true;
}

/// Canonicalize every value and drop the pairs that do not survive.
///
/// A pair is dropped when its canonical value is empty, or when its name is
/// identifying and the value is an unprogrammed placeholder. Both filters run
/// before the duplicate check, so two entries sharing a name where one is empty
/// are not a duplicate.
///
/// \throws duplicate_fingerprint_parameter_error when two surviving pairs share a
///         name. The material grammar cannot express it, so it is a collection bug.
[[nodiscard]] inline parameter_list canonicalize_params(const parameter_list& params)
{
    parameter_list kept;
    kept.reserve(params.size());

    for (const auto& param : params) {
        // Not a structured binding: capturing one in a lambda is C++20, and this
        // header has to compile as C++17 on every supported compiler.
        const std::string& name = param.first;

        auto value = canonicalize_value(param.second);
        if (value.empty()) {
            continue;
        }
        if (is_identifying_param(name) && !is_usable_identity(name, value)) {
            continue;
        }

        const bool already_kept = std::any_of(
            kept.begin(), kept.end(), [&name](const parameter& kept_param) { return kept_param.first == name; });
        if (already_kept) {
            throw duplicate_fingerprint_parameter_error(name);
        }

        kept.emplace_back(name, std::move(value));
    }

    return kept;
}

/// Assemble the material to be hashed.
///
/// Lines are **joined** by a single LF, never terminated by one: the material
/// does not end with a newline. Appending "\n" after each line is the single most
/// likely way to produce an SDK that looks correct and agrees with nothing, and
/// the vectors check it explicitly.
///
/// \throws insufficient_device_identity_error when nothing survives
///         canonicalization, or when nothing that survives identifies this
///         individual machine. Neither may be hashed anyway: each would hand a
///         whole class of machines the same device id.
/// \throws duplicate_fingerprint_parameter_error via canonicalize_params.
[[nodiscard]] inline std::string build_fingerprint_material(
    std::string_view platform,
    const parameter_list& params)
{
    const auto kept = canonicalize_params(params);
    if (kept.empty()) {
        throw insufficient_device_identity_error(std::string(platform));
    }

    // Enforced here rather than in the resolver so it also binds a custom reader
    // and a native bridge assembling material directly. On these platforms the
    // host name is a constant (since iOS 17 gethostname() returns "localhost", and
    // UIDevice.name the model name), so accepting it would be worse than failing:
    // one activation would validate across the whole install base.
    const bool scoped_platform = platform == "ios" || platform == "android";
    const bool has_device_name = std::any_of(
        kept.begin(), kept.end(), [](const parameter& param) { return param.first == "deviceName"; });
    if (scoped_platform && has_device_name) {
        throw insufficient_device_identity_error(
            std::string(platform),
            "the host-name fallback is not available on this platform, where the host name is the"
            " same on every device");
    }

    const bool identifies_this_machine = std::any_of(
        kept.begin(), kept.end(), [](const parameter& param) { return is_identifying_param(param.first); });
    if (!identifies_this_machine) {
        std::string names;
        for (const auto& [name, value] : kept) {
            if (!names.empty()) {
                names += ", ";
            }
            names += name;
        }
        throw insufficient_device_identity_error(
            std::string(platform),
            "only model-level parameters could be read (" + names
                + "), none of which identify this individual machine");
    }

    std::string material;
    material.append(prefix);
    material += "\nplatform=";
    material.append(platform);
    for (const auto& [name, value] : kept) {
        material += '\n';
        material += name;
        material += '=';
        material += value;
    }

    return material;
}

/// Hash material into a bare digest: 64 lowercase hex characters of SHA-256.
[[nodiscard]] inline std::string fingerprint_digest(std::string_view material)
{
    return moonbase::detail::sha256_hex(material);
}

/// Prefix a digest with its version and source, producing the wire-form device id.
[[nodiscard]] inline std::string stamp_device_id(
    std::string_view digest,
    device_id_source source = device_id_source::identity)
{
    std::string out = "mbd";
    out += std::to_string(version);
    out.append(internal::tag_for_source(source));
    out += '_';
    out.append(digest);
    return out;
}

/// Hash material and stamp it: the device id sent to Moonbase and stored in `sig`.
[[nodiscard]] inline std::string fingerprint_device_id(
    std::string_view material,
    device_id_source source = device_id_source::identity)
{
    return stamp_device_id(fingerprint_digest(material), source);
}

/// Recover the version and source from a device id, or nothing if it is not a
/// Moonbase stamp.
///
/// Because the version is recoverable from the id, a validator can tell an
/// out-of-date SDK (the binding is newer than what it computes) from a stale
/// binding (the binding is older), and say something better than "wrong device".
/// A bare digest, a custom resolver's id, or an id from an SDK predating
/// versioned fingerprints all return nothing and are compared literally.
///
/// Strict by design: uppercase hex, a truncated digest and a missing separator
/// all fail to parse rather than being coerced.
[[nodiscard]] inline std::optional<device_id_stamp> parse_device_id_stamp(std::string_view device_id)
{
    constexpr std::string_view lead = "mbd";
    if (device_id.size() < lead.size() || device_id.substr(0, lead.size()) != lead) {
        return std::nullopt;
    }

    std::size_t cursor = lead.size();
    const std::size_t digits_begin = cursor;
    while (cursor != device_id.size() && device_id[cursor] >= '0' && device_id[cursor] <= '9') {
        ++cursor;
    }

    const std::size_t digit_count = cursor - digits_begin;
    // At least one digit, and few enough that the value cannot overflow an int.
    // JavaScript would happily produce a float for a 40-digit version; no
    // validator will ever see one, and refusing is the safer disagreement.
    if (digit_count == 0 || digit_count > 9) {
        return std::nullopt;
    }

    // [a-z]*, not a fixed set: a tag this SDK does not define must still parse, so
    // that a newer SDK can introduce one without a version bump. Digits cannot
    // appear in it and '_' terminates it, so the split from the version is
    // unambiguous. Uppercase is not a tag, so mbd2S_ correctly fails to parse.
    const std::size_t tag_begin = cursor;
    while (cursor != device_id.size() && device_id[cursor] >= 'a' && device_id[cursor] <= 'z') {
        ++cursor;
    }
    const auto source_tag = device_id.substr(tag_begin, cursor - tag_begin);

    if (cursor == device_id.size() || device_id[cursor] != '_') {
        return std::nullopt;
    }
    ++cursor;

    const auto digest = device_id.substr(cursor);
    if (digest.size() != 64 || !internal::is_lowercase_hex(digest)) {
        return std::nullopt;
    }

    int parsed_version = 0;
    for (std::size_t index = digits_begin; index != digits_begin + digit_count; ++index) {
        parsed_version = parsed_version * 10 + (device_id[index] - '0');
    }

    return device_id_stamp{
        parsed_version,
        std::string(source_tag),
        internal::source_for_tag(source_tag),
        std::string(digest)};
}

// ---------------------------------------------------------------------------
// Source parsers. Pure, so the platform readers stay thin and every one of these
// is tested on every platform.

/// Normalize a platform UUID for the material: hyphens removed, uppercased.
[[nodiscard]] inline std::string normalize_platform_uuid(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (const char character : raw) {
        if (character == '-') {
            continue;
        }
        out.push_back(
            (character >= 'a' && character <= 'z') ? static_cast<char>(character - 'a' + 'A') : character);
    }
    return out;
}

/// Extract IOPlatformUUID from `ioreg -rd1 -c IOPlatformExpertDevice` output.
///
/// Kept for consumers that already have ioreg output; the resolver itself reads
/// IOKit directly, which works inside the App Sandbox where spawning a process
/// does not.
[[nodiscard]] inline std::string parse_ioreg_platform_uuid(std::string_view ioreg_output)
{
    constexpr std::string_view key = "\"IOPlatformUUID\"";
    const auto key_at = ioreg_output.find(key);
    if (key_at == std::string_view::npos) {
        return {};
    }

    const auto skip_spaces = [&ioreg_output](std::size_t from) {
        while (from != ioreg_output.size()
               && (ioreg_output[from] == ' ' || ioreg_output[from] == '\t' || ioreg_output[from] == '\r'
                   || ioreg_output[from] == '\n')) {
            ++from;
        }
        return from;
    };

    auto cursor = skip_spaces(key_at + key.size());
    if (cursor == ioreg_output.size() || ioreg_output[cursor] != '=') {
        return {};
    }
    cursor = skip_spaces(cursor + 1);
    if (cursor == ioreg_output.size() || ioreg_output[cursor] != '"') {
        return {};
    }
    ++cursor;

    const auto end = ioreg_output.find('"', cursor);
    if (end == std::string_view::npos || end == cursor) {
        return {};
    }

    return normalize_platform_uuid(ioreg_output.substr(cursor, end - cursor));
}

/// Does this canonical value look like a real machine-id(5)?
[[nodiscard]] inline bool is_valid_machine_id(std::string_view canonical_value) noexcept
{
    return canonical_value.size() == 32 && internal::is_lowercase_hex(canonical_value)
        && !is_not_programmed(canonical_value);
}

/// Pick the first machine-id source holding a valid id.
///
/// Each source is validated before selection rather than taking the first
/// non-empty one. /etc/machine-id legitimately holds the literal marker
/// "uninitialized" in an initrd or a golden image awaiting first boot, and every
/// machine deployed from that image reads the same marker. Treating it as an id
/// would give them all one device id, and would also stop the fall-through to a
/// D-Bus id that may be perfectly valid.
[[nodiscard]] inline std::string select_machine_id(std::initializer_list<std::string_view> sources)
{
    for (const auto source : sources) {
        auto candidate = canonicalize_value(source);
        if (is_valid_machine_id(candidate)) {
            return candidate;
        }
    }
    return {};
}

namespace internal {

struct smbios_structure {
    unsigned char type = 0;
    /// The formatted area, indexed from the structure header (byte 0 = type).
    const unsigned char* formatted = nullptr;
    std::size_t formatted_size = 0;
    /// Resolved string table; a string-index field holding N maps to strings[N - 1].
    std::vector<std::string> strings;
};

// Firmware string bytes are decoded as Latin-1, matching the reference SDK's
// Buffer.toString('latin1'). SMBIOS strings are nominally ASCII but OEMs ship
// worse, and canonicalization discards everything above U+007E anyway, so the
// choice is very nearly immaterial. Very nearly: it matters when firmware bytes
// happen to form a valid UTF-8 combining mark after an ASCII byte, where a UTF-8
// decoder would let the mark annihilate that character and a Latin-1 decoder
// would not. Matching the reference removes the last way two conforming SDKs
// could disagree.
[[nodiscard]] inline std::string latin1_to_utf8(const unsigned char* data, std::size_t size)
{
    std::string out;
    out.reserve(size);
    for (std::size_t index = 0; index != size; ++index) {
        const unsigned char byte = data[index];
        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte));
        } else {
            out.push_back(static_cast<char>(0xC0U | (byte >> 6U)));
            out.push_back(static_cast<char>(0x80U | (byte & 0x3FU)));
        }
    }
    return out;
}

/// Walk an SMBIOS structure table (with no leading RawSMBIOSData header).
[[nodiscard]] inline std::vector<smbios_structure> parse_smbios_structures(
    const unsigned char* data,
    std::size_t size)
{
    std::vector<smbios_structure> structures;
    std::size_t offset = 0;

    while (offset + 4 <= size) {
        const unsigned char type = data[offset];
        const std::size_t length = data[offset + 1];
        // A structure shorter than its own header, or one running off the end of
        // the table, means the table is malformed from here on.
        if (length < 4 || offset + length > size) {
            break;
        }

        smbios_structure structure;
        structure.type = type;
        structure.formatted = data + offset;
        structure.formatted_size = length;

        // The string table follows the formatted area: NUL-terminated strings
        // ending in a double-NUL. A structure with no strings is just the
        // double-NUL.
        std::size_t cursor = offset + length;
        if (cursor + 1 < size && data[cursor] == 0 && data[cursor + 1] == 0) {
            cursor += 2;
        } else {
            while (cursor < size) {
                std::size_t end = cursor;
                while (end < size && data[end] != 0) {
                    ++end;
                }
                structure.strings.push_back(latin1_to_utf8(data + cursor, end - cursor));
                cursor = end + 1;
                if (cursor < size && data[cursor] == 0) {
                    cursor += 1;
                    break;
                }
            }
        }

        structures.push_back(std::move(structure));

        if (type == 127) { // End-of-table.
            break;
        }

        offset = cursor;
    }

    return structures;
}

/// Resolve a string-index field. Index 0, or one past the end of the table,
/// means "no string".
[[nodiscard]] inline std::string resolve_smbios_string(
    const smbios_structure& structure,
    std::size_t field_offset)
{
    // Bounded by the structure's own length, not by the size of the table. Older
    // SMBIOS 2.x structures are shorter than the current layout, and reading past
    // the formatted area silently picks up bytes from the string pool and
    // resolves a garbage index.
    if (field_offset >= structure.formatted_size) {
        return {};
    }

    const std::size_t index = structure.formatted[field_offset];
    if (index == 0 || index > structure.strings.size()) {
        return {};
    }

    return structure.strings[index - 1];
}

/// Format a 16-byte UUID field as uppercase hex.
///
/// The raw bytes in order: no hyphens, and deliberately **not** the
/// SMBIOS-canonical little-endian swap of the first three fields. The value will
/// therefore not match what dmidecode or Win32_ComputerSystemProduct display,
/// which is intentional and specified. An SDK reading the UUID through WMI must
/// undo that swap.
[[nodiscard]] inline std::string format_smbios_uuid(
    const smbios_structure& structure,
    std::size_t field_offset)
{
    if (field_offset + 16 > structure.formatted_size) {
        return {};
    }

    const unsigned char* bytes = structure.formatted + field_offset;

    // All-00 or all-FF means "not set", so fleets of VMs with unset UUIDs cannot
    // collide on one device id.
    bool all_zeroes = true;
    bool all_ones = true;
    for (std::size_t index = 0; index != 16; ++index) {
        all_zeroes = all_zeroes && bytes[index] == 0x00;
        all_ones = all_ones && bytes[index] == 0xFF;
    }
    if (all_zeroes || all_ones) {
        return {};
    }

    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(32);
    for (std::size_t index = 0; index != 16; ++index) {
        out.push_back(hex[bytes[index] >> 4U]);
        out.push_back(hex[bytes[index] & 0x0FU]);
    }
    return out;
}

} // namespace internal

/// Extract the Windows identity parameters from a raw SMBIOS structure table.
///
/// Takes the first type-1 (System) and the first type-2 (Baseboard) structure;
/// later structures of the same type are ignored. Type 4 (Processor) is
/// deliberately not collected: its values are model-level rather than
/// per-machine, and the number of type-4 structures tracks the CPU socket or
/// vCPU count, so collecting them would change the device id every time a VM is
/// resized.
///
/// Parameters are emitted even when their value is empty. Describing the
/// firmware is this function's job; deciding what counts is canonicalization's.
[[nodiscard]] inline parameter_list parse_smbios_params(const unsigned char* data, std::size_t size)
{
    parameter_list params;
    if (data == nullptr || size == 0) {
        return params;
    }

    const auto structures = internal::parse_smbios_structures(data, size);
    const auto find_first = [&structures](unsigned char type) {
        return std::find_if(structures.begin(), structures.end(), [type](const internal::smbios_structure& s) {
            return s.type == type;
        });
    };

    if (const auto system = find_first(1); system != structures.end()) {
        params.emplace_back("systemManufacturer", internal::resolve_smbios_string(*system, 0x04));
        params.emplace_back("systemProductName", internal::resolve_smbios_string(*system, 0x05));
        params.emplace_back("systemUuid", internal::format_smbios_uuid(*system, 0x08));
    }

    if (const auto baseboard = find_first(2); baseboard != structures.end()) {
        params.emplace_back("baseboardManufacturer", internal::resolve_smbios_string(*baseboard, 0x04));
        params.emplace_back("baseboardProduct", internal::resolve_smbios_string(*baseboard, 0x05));
        params.emplace_back("baseboardSerialNumber", internal::resolve_smbios_string(*baseboard, 0x07));
    }

    return params;
}

[[nodiscard]] inline parameter_list parse_smbios_params(const std::vector<unsigned char>& data)
{
    return parse_smbios_params(data.data(), data.size());
}

} // namespace moonbase::fingerprint_spec
