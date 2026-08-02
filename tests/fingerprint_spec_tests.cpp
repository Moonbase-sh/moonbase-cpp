// Conformance suite for the Moonbase device fingerprint spec.
//
// Driven by tests/vectors/fingerprint-vectors.json, vendored verbatim from
// @moonbase.sh/licensing. The spec prose is normative but the vectors are
// decisive: they are what every SDK can actually execute, so when the two
// disagree the vectors win.
//
// Everything exercised here is pure, so these cases run identically on Linux,
// macOS and Windows. That is the point of keeping the algorithm free of OS
// headers: the Windows SMBIOS parser and the macOS ioreg parser are covered on
// every runner, not only where they happen to execute.
//
// What this file cannot prove is that the platform *readers* agree with the
// reference SDK on real hardware, because those are the half that touches the
// machine. .github/workflows/fingerprint-parity.yml closes that gap.

#include <atomic>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "moonbase/errors.hpp"
#include "moonbase/fingerprint_spec.hpp"

#ifndef MOONBASE_FINGERPRINT_VECTORS_PATH
#error "MOONBASE_FINGERPRINT_VECTORS_PATH must be defined (see CMakeLists.txt)"
#endif

namespace fp = moonbase::fingerprint_spec;

namespace {

const nlohmann::json& vectors()
{
    static const nlohmann::json loaded = [] {
        std::ifstream file(MOONBASE_FINGERPRINT_VECTORS_PATH);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " MOONBASE_FINGERPRINT_VECTORS_PATH);
        nlohmann::json json;
        file >> json;
        return json;
    }();
    return loaded;
}

/// Byte dump, so a failure involving invisible characters is readable.
std::string describe_bytes(const std::string& value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char byte : value) {
        if (byte >= 0x20 && byte <= 0x7E) {
            out.push_back(static_cast<char>(byte));
        } else {
            out += "\\x";
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0x0FU]);
        }
    }
    return out;
}

std::vector<unsigned char> decode_hex(const std::string& text)
{
    const auto nibble = [](char character) {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        return (character | 0x20) - 'a' + 10;
    };

    std::vector<unsigned char> out;
    out.reserve(text.size() / 2);
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) {
        out.push_back(static_cast<unsigned char>(nibble(text[index]) * 16 + nibble(text[index + 1])));
    }
    return out;
}

fp::device_id_source source_from_name(const std::string& name)
{
    if (name == "identity") return fp::device_id_source::identity;
    if (name == "deviceName") return fp::device_id_source::device_name;
    if (name == "scoped") return fp::device_id_source::scoped;
    FAIL("unknown source in vectors: " << name);
    return fp::device_id_source::identity;
}

fp::parameter_list to_params(const nlohmann::json& json)
{
    fp::parameter_list params;
    for (const auto& pair : json) {
        params.emplace_back(pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
    }
    return params;
}

} // namespace

TEST_CASE("vectors pin the spec version this SDK implements")
{
    // The cross-SDK drift alarm. If the vendored vector file moves to a new spec
    // version, this fails before any individual vector does, which is a much
    // clearer signal than a wall of digest mismatches.
    CHECK(vectors().at("version").get<int>() == fp::version);
    CHECK(vectors().at("materialPrefix").get<std::string>() == fp::prefix);

    const auto stamp_prefix = vectors().at("stampPrefix").get<std::string>();
    CHECK(fp::stamp_device_id(std::string(64, 'a')).rfind(stamp_prefix, 0) == 0);

    // Every tag the vectors define must be the one this SDK emits for that source.
    const auto digest = std::string(64, 'a');
    for (const auto& [name, tag] : vectors().at("sourceTags").items()) {
        INFO("source: " << name);
        CHECK(fp::stamp_device_id(digest, source_from_name(name))
            == "mbd" + std::to_string(fp::version) + tag.get<std::string>() + "_" + digest);
    }
}

TEST_CASE("canonicalizeValue vectors")
{
    for (const auto& vector : vectors().at("canonicalizeValue")) {
        const auto input = vector.at("input").get<std::string>();
        const auto expected = vector.at("expected").get<std::string>();

        INFO("vector: " << vector.at("description").get<std::string>());
        INFO("input:  " << describe_bytes(input));
        CHECK(fp::canonicalize_value(input) == expected);
    }
}

TEST_CASE("material vectors")
{
    for (const auto& vector : vectors().at("material")) {
        const auto platform = vector.at("platform").get<std::string>();
        const auto expected_device_id = vector.at("deviceId").get<std::string>();
        // Read the declared source rather than sniffing the stamp: a prefix sniff
        // silently mislabels any tag it was not taught about.
        const auto source = source_from_name(vector.at("source").get<std::string>());

        INFO("vector: " << vector.at("description").get<std::string>());

        const auto material = fp::build_fingerprint_material(platform, to_params(vector.at("params")));
        CHECK(material == vector.at("material").get<std::string>());
        CHECK(fp::fingerprint_digest(material) == vector.at("digest").get<std::string>());
        CHECK(fp::fingerprint_device_id(material, source) == expected_device_id);
    }
}

TEST_CASE("material is LF-joined and never LF-terminated")
{
    // Its own case, because appending a newline after every line is the single
    // most likely way to produce an SDK that looks correct and agrees with
    // nothing, and a bare digest mismatch would not say so.
    for (const auto& vector : vectors().at("material")) {
        const auto material = fp::build_fingerprint_material(
            vector.at("platform").get<std::string>(), to_params(vector.at("params")));

        INFO("vector: " << vector.at("description").get<std::string>());
        REQUIRE(!material.empty());
        CHECK(material.back() != '\n');
    }
}

TEST_CASE("materialErrors vectors")
{
    for (const auto& vector : vectors().at("materialErrors")) {
        const auto platform = vector.at("platform").get<std::string>();
        const auto params = to_params(vector.at("params"));
        const auto kind = vector.at("error").get<std::string>();

        INFO("vector: " << vector.at("description").get<std::string>());
        INFO("expected error: " << kind);

        if (kind == "InsufficientDeviceIdentity") {
            CHECK_THROWS_AS(
                fp::build_fingerprint_material(platform, params),
                moonbase::insufficient_device_identity_error);
        } else if (kind == "DuplicateParameter") {
            CHECK_THROWS_AS(
                fp::build_fingerprint_material(platform, params),
                moonbase::duplicate_fingerprint_parameter_error);
        } else {
            FAIL("unknown error kind in vectors: " << kind);
        }
    }
}

TEST_CASE("smbios vectors")
{
    for (const auto& vector : vectors().at("smbios")) {
        const auto table = decode_hex(vector.at("table").get<std::string>());
        const auto expected = to_params(vector.at("params"));
        const auto actual = fp::parse_smbios_params(table);

        INFO("vector: " << vector.at("description").get<std::string>());

        REQUIRE(actual.size() == expected.size());
        for (std::size_t index = 0; index != expected.size(); ++index) {
            INFO("parameter " << index);
            CHECK(actual[index].first == expected[index].first);
            // Compared including empty values: describing the firmware is the
            // parser's job, and deciding what counts is canonicalization's.
            CHECK(actual[index].second == expected[index].second);
        }
    }
}

TEST_CASE("stamp vectors")
{
    for (const auto& vector : vectors().at("stamps")) {
        const auto device_id = vector.at("deviceId").get<std::string>();
        const auto parsed = fp::parse_device_id_stamp(device_id);

        INFO("vector: " << vector.at("description").get<std::string>());

        if (vector.at("parsed").is_null()) {
            CHECK(!parsed.has_value());
            continue;
        }

        REQUIRE(parsed.has_value());
        const auto& expected = vector.at("parsed");
        CHECK(parsed->version == expected.at("version").get<int>());
        CHECK(parsed->digest == expected.at("digest").get<std::string>());
        CHECK(parsed->source_tag == expected.at("sourceTag").get<std::string>());

        if (expected.at("source").is_null()) {
            // A tag a newer SDK introduced: it must still parse, with the literal
            // tag preserved and no meaning attached.
            CHECK(!parsed->source.has_value());
        } else {
            REQUIRE(parsed->source.has_value());
            CHECK(*parsed->source == source_from_name(expected.at("source").get<std::string>()));
        }
    }
}

TEST_CASE("digest is SHA-256 over UTF-8, lowercase hex")
{
    // Pins all three interchangeable crypto backends (OpenSSL, Security.framework,
    // CNG) to one well-known vector. More valuable here than in a single-backend
    // SDK: a backend that disagreed would silently invalidate every license.
    CHECK(fp::fingerprint_digest("abc")
        == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("scoped identity is stamped and round-trips")
{
    // A third source tag alongside the hardware id and the host-name fallback, for
    // platforms whose only device identifier is scoped to the publisher (iOS
    // identifierForVendor, Android ANDROID_ID). The stamp is the whole point: it
    // travels with the value so a validator, a server and an analytics pipeline all
    // know the id must not be correlated across publishers.
    const auto digest = std::string(64, 'a');
    const auto scoped = fp::stamp_device_id(digest, fp::device_id_source::scoped);

    CHECK(scoped == "mbd2s_" + digest);
    CHECK(scoped.size() == 70);

    const auto parsed = fp::parse_device_id_stamp(scoped);
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == fp::version);
    CHECK(parsed->source == fp::device_id_source::scoped);
    CHECK(parsed->digest == digest);

    SUBCASE("the three source tags are distinct ids for the same digest")
    {
        CHECK(fp::stamp_device_id(digest, fp::device_id_source::identity) != scoped);
        CHECK(fp::stamp_device_id(digest, fp::device_id_source::device_name) != scoped);
    }

    SUBCASE("an unknown source tag parses, with no meaning attached")
    {
        // Rejecting it would report a perfectly valid id from a newer SDK as "not a
        // Moonbase device id". The tag is preserved literally so a diagnostic can
        // name it; only its *meaning* is unknown.
        const auto unknown = fp::parse_device_id_stamp("mbd2x_" + digest);
        REQUIRE(unknown.has_value());
        CHECK(unknown->source_tag == "x");
        CHECK(!unknown->source.has_value());
        CHECK(unknown->version == 2);

        // [a-z]*, so a future two-letter tag needs no version bump either.
        const auto two_letter = fp::parse_device_id_stamp("mbd2zz_" + digest);
        REQUIRE(two_letter.has_value());
        CHECK(two_letter->source_tag == "zz");

        // Uppercase is not a tag, and a digit would make the version ambiguous.
        CHECK(!fp::parse_device_id_stamp("mbd2S_" + digest).has_value());
        CHECK(!fp::parse_device_id_stamp("mbd2n2_" + digest).has_value());
    }
}

TEST_CASE("scoped platforms build spec-shaped material")
{
    // iOS: one identifying parameter, hyphens stripped and uppercased exactly as
    // ioPlatformUuid is, so the two platforms stay consistent.
    const auto material = fp::build_fingerprint_material(
        "ios", {{"identifierForVendor", "C1234567-89AB-CDEF-0123-456789ABCDEF"}});
    CHECK(material
        == "moonbase:fingerprint:v2\nplatform=ios\nidentifierForVendor=C1234567-89AB-CDEF-0123-456789ABCDEF");

    CHECK(fp::is_identifying_param("identifierForVendor"));
    CHECK(fp::is_identifying_param("androidId"));

    SUBCASE("an absent scoped identifier is insufficient identity, not a constant")
    {
        // identifierForVendor is nil until first unlock after boot, and ANDROID_ID
        // is empty before user setup. Hashing either would give every device in that
        // state one shared id.
        CHECK_THROWS_AS(
            fp::build_fingerprint_material("ios", {{"identifierForVendor", ""}}),
            moonbase::insufficient_device_identity_error);
        CHECK_THROWS_AS(
            fp::build_fingerprint_material("android", {{"androidId", ""}}),
            moonbase::insufficient_device_identity_error);
    }
}

TEST_CASE("identifying parameters are fixed, in order, and not widenable")
{
    const std::vector<std::string_view> expected{
        "ioPlatformUuid", "machineId", "systemUuid", "baseboardSerialNumber",
        "identifierForVendor", "androidId", "deviceName"};
    CHECK(fp::identifying_param_names() == expected);

    // Returned by value, so a consumer cannot turn "this model has no identity"
    // into "it does" for the rest of the process.
    auto names = fp::identifying_param_names();
    names.emplace_back("sysVendor");
    CHECK_THROWS_AS(
        fp::build_fingerprint_material("linux", {{"sysVendor", "LENOVO"}}),
        moonbase::insufficient_device_identity_error);
    CHECK(!fp::is_identifying_param("sysVendor"));
}

TEST_CASE("NFC runs before the ASCII filter")
{
    // None of these are in the vector file, and every one of them is a case a
    // naive "strip non-ASCII" port gets wrong. Derived exhaustively by
    // scripts/gen-nfc-tables.py; see detail/unicode/nfc_ascii.hpp.
    SUBCASE("singleton decompositions expose a printable ASCII character")
    {
        CHECK(fp::canonicalize_value("\xE2\x84\xAA") == "K");  // U+212A KELVIN SIGN
        CHECK(fp::canonicalize_value("\xCD\xBE") == ";");      // U+037E GREEK QUESTION MARK
        CHECK(fp::canonicalize_value("\xE1\xBF\xAF") == "`");  // U+1FEF GREEK VARIA
    }

    SUBCASE("a combining mark annihilates the ASCII base it composes with")
    {
        CHECK(fp::canonicalize_value("cafe\xCC\x81") == "caf");  // the spec's worked example
        CHECK(fp::canonicalize_value("=\xCC\xB8") == "");        // '=' + U+0338 -> U+2260
    }

    SUBCASE("marks that decompose are expanded first")
    {
        CHECK(fp::canonicalize_value("A\xCD\x81") == "");  // U+0341 -> U+0301
        CHECK(fp::canonicalize_value("e\xCD\x84") == "");  // U+0344 -> U+0308 U+0301
    }

    SUBCASE("a mark that does not compose leaves the base alone")
    {
        CHECK(fp::canonicalize_value("e\xCC\x85") == "e");  // U+0305 has no composite with 'e'
    }

    SUBCASE("blocking depends on order, not just membership")
    {
        // Same two marks, opposite order. U+0305 and U+0301 share combining class
        // 230, so whichever comes first blocks the other.
        CHECK(fp::canonicalize_value("e\xCC\x85\xCC\x81") == "e");
        CHECK(fp::canonicalize_value("e\xCC\x81\xCC\x85") == "");
    }
}

TEST_CASE("malformed UTF-8 is dropped without swallowing neighbours")
{
    // Firmware strings are the realistic source. Exactly one byte is consumed per
    // malformed byte, so a bad prefix cannot eat the character after it, and a
    // truncated combining mark cannot annihilate the character before it.
    CHECK(fp::canonicalize_value("AB\xC3") == "AB");             // truncated 2-byte lead
    CHECK(fp::canonicalize_value("A\xFF" "B") == "AB");          // never a valid lead
    CHECK(fp::canonicalize_value("A\xC1\xA0" "B") == "AB");      // overlong 'a'
    CHECK(fp::canonicalize_value("A\xED\xA0\x80" "B") == "AB");  // encoded surrogate
    CHECK(fp::canonicalize_value("A\x80" "B") == "AB");          // bare continuation byte
    CHECK(fp::canonicalize_value("e\xCC") == "e");               // truncated combining mark
}

TEST_CASE("unprogrammed placeholders are recognised case-insensitively")
{
    for (const char* filler : {"to be filled by o.e.m.",
                               "TO BE FILLED BY O.E.M.",
                               "To Be Filled By OEM",
                               "Default string",
                               "DEFAULT STRING",
                               "system serial number",
                               "Base Board Serial Number",
                               "chassis serial number",
                               "Not Specified",
                               "not applicable",
                               "Not Available",
                               "None",
                               "unknown",
                               "Invalid",
                               "N/A",
                               "0123456789",
                               "uninitialized"}) {
        INFO("filler: " << filler);
        CHECK(fp::is_not_programmed(filler));
    }

    SUBCASE("all-zero and all-f values are placeholders too")
    {
        CHECK(fp::is_not_programmed("00000000000000000000000000000000"));
        CHECK(fp::is_not_programmed("ffffffffffffffffffffffffffffffff"));
        CHECK(fp::is_not_programmed("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));
        CHECK(fp::is_not_programmed("0"));
    }

    SUBCASE("an empty value is not a placeholder")
    {
        // It is dropped by the empty rule instead. Reporting it here would make
        // the all-zero test above accidentally match everything.
        CHECK(!fp::is_not_programmed(""));
    }

    SUBCASE("real values are not placeholders")
    {
        CHECK(!fp::is_not_programmed("BSN-42"));
        CHECK(!fp::is_not_programmed("b08dfa6083e7567a1921a715000001fb"));
        CHECK(!fp::is_not_programmed("none of your business"));
    }

    SUBCASE("the rule applies to identifying parameters only")
    {
        // A model may genuinely be named "None"; a serial number reading it is
        // not a serial number.
        const auto kept = fp::canonicalize_params({{"machineId", "b08dfa60"}, {"productName", "None"}});
        REQUIRE(kept.size() == 2);
        CHECK(kept[1].first == "productName");

        const auto dropped =
            fp::canonicalize_params({{"machineId", "b08dfa60"}, {"baseboardSerialNumber", "None"}});
        CHECK(dropped.size() == 1);
    }
}

TEST_CASE("duplicate detection runs after the drop filters")
{
    // Two entries sharing a name are not a duplicate when one of them does not
    // survive, which is what lets a reader emit a parameter unconditionally.
    CHECK_NOTHROW(fp::canonicalize_params({{"systemUuid", ""}, {"systemUuid", "AAAA"}}));
    CHECK_THROWS_AS(
        fp::canonicalize_params({{"systemUuid", "AAAA"}, {"systemUuid", "BBBB"}}),
        moonbase::duplicate_fingerprint_parameter_error);
}

TEST_CASE("insufficient identity distinguishes nothing-read from model-only")
{
    using moonbase::insufficient_device_identity_error;

    try {
        (void)fp::build_fingerprint_material("linux", {});
        FAIL("expected insufficient_device_identity_error");
    } catch (const insufficient_device_identity_error& ex) {
        CHECK(ex.platform() == "linux");
        CHECK(std::string(ex.what()).find("platform: linux") != std::string::npos);
    }

    try {
        (void)fp::build_fingerprint_material("linux", {{"sysVendor", "LENOVO"}, {"boardName", "20HRCTO1WW"}});
        FAIL("expected insufficient_device_identity_error");
    } catch (const insufficient_device_identity_error& ex) {
        // The message names the parameters that did read, because "no identity"
        // on a machine that clearly reported something is otherwise baffling.
        CHECK(ex.reason().find("sysVendor") != std::string::npos);
        CHECK(ex.reason().find("boardName") != std::string::npos);
        CHECK(ex.reason().find("model-level") != std::string::npos);
    }
}
