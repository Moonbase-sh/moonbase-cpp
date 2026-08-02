// The reader side of the device fingerprint: the source parsers, and the
// resolver's memoization.
//
// The parsers are pure and compiled on every platform, so the SMBIOS walker is
// fuzzed on the Linux sanitizer runner even though it only ever runs on Windows.
// That matters more than it sounds: it is the only binary blob this SDK parses
// that it did not write, and it is handed straight from firmware.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "moonbase/errors.hpp"
#include "moonbase/fingerprint_spec.hpp"
#include "moonbase/moonbase_device_id_resolver.hpp"

namespace fp = moonbase::fingerprint_spec;

TEST_CASE("parse_ioreg_platform_uuid strips hyphens and uppercases")
{
    CHECK(fp::parse_ioreg_platform_uuid(
              "    | |   \"IOPlatformUUID\" = \"c1234567-89ab-cdef-0123-456789abcdef\"\n")
        == "C123456789ABCDEF0123456789ABCDEF");

    SUBCASE("absent, empty or malformed input yields nothing")
    {
        CHECK(fp::parse_ioreg_platform_uuid("nothing here").empty());
        CHECK(fp::parse_ioreg_platform_uuid("").empty());
        CHECK(fp::parse_ioreg_platform_uuid("\"IOPlatformUUID\" = \"\"").empty());
        CHECK(fp::parse_ioreg_platform_uuid("\"IOPlatformUUID\" = \"unterminated").empty());
        CHECK(fp::parse_ioreg_platform_uuid("\"IOPlatformUUID\"").empty());
    }

    SUBCASE("whitespace around the assignment is tolerated")
    {
        CHECK(fp::parse_ioreg_platform_uuid("\"IOPlatformUUID\"\n\t=  \"abc-def\"") == "ABCDEF");
    }
}

TEST_CASE("select_machine_id validates each source before choosing it")
{
    const std::string valid = "b08dfa6083e7567a1921a715000001fb";
    const std::string other = "ffc0ffee83e7567a1921a715000001fb";

    CHECK(fp::select_machine_id({valid}) == valid);
    CHECK(fp::select_machine_id({"b08dfa6083e7567a1921a715000001fb\n"}) == valid);

    SUBCASE("an invalid first source falls through instead of stranding the rest")
    {
        // /etc/machine-id legitimately holds "uninitialized" in an initrd or a
        // golden image awaiting first boot. Taking the first non-empty source
        // would give every machine deployed from that image one device id, and
        // would also hide a perfectly good D-Bus id.
        CHECK(fp::select_machine_id({"uninitialized\n", other + "\n"}) == other);
        CHECK(fp::select_machine_id({std::string(32, '0'), other}) == other);
        CHECK(fp::select_machine_id({"", other}) == other);
    }

    SUBCASE("rejected forms")
    {
        for (const std::string candidate : {
                 std::string("uninitialized"),
                 std::string(32, '0'),
                 std::string(32, 'f'),
                 std::string(32, 'F'),
                 std::string(31, 'a'),                          // too short
                 std::string(33, 'a'),                          // too long
                 std::string("B08DFA6083E7567A1921A715000001FB"),  // uppercase
                 std::string("g08dfa6083e7567a1921a715000001fb"),  // non-hex
                 std::string("b08dfa60-83e7-567a-1921-a715000001fb"),  // hyphenated
                 std::string(""),
             }) {
            INFO("candidate: " << candidate);
            CHECK(fp::select_machine_id({candidate}).empty());
        }
    }

    SUBCASE("selection and canonicalization never disagree")
    {
        // The invariant that stops the two rules drifting: a value select_machine_id
        // accepts must be one canonicalize_params would keep, or the machine ends
        // up with an id built from a parameter the material then discards.
        for (const std::string candidate : {
                 std::string("b08dfa6083e7567a1921a715000001fb"),
                 std::string(32, 'f'),
                 std::string("uninitialized"),
                 std::string(32, '0'),
                 std::string("g08dfa6083e7567a1921a715000001fb"),
             }) {
            INFO("candidate: " << candidate);
            const bool selected = !fp::select_machine_id({candidate}).empty();
            const bool kept = !fp::canonicalize_params({{"machineId", candidate}}).empty();
            CHECK(selected <= kept);
        }
    }
}

TEST_CASE("parse_smbios_params survives malformed tables")
{
    SUBCASE("degenerate buffers")
    {
        CHECK(fp::parse_smbios_params(nullptr, 0).empty());
        CHECK(fp::parse_smbios_params(std::vector<unsigned char>{}).empty());
        CHECK(fp::parse_smbios_params(std::vector<unsigned char>{0x01}).empty());
        CHECK(fp::parse_smbios_params(std::vector<unsigned char>{0x01, 0x1B, 0x00}).empty());
    }

    SUBCASE("a length below the header size stops the walk")
    {
        CHECK(fp::parse_smbios_params(std::vector<unsigned char>{0x01, 0x03, 0x00, 0x00}).empty());
    }

    SUBCASE("a length running past the end stops the walk")
    {
        CHECK(fp::parse_smbios_params(std::vector<unsigned char>{0x01, 0x40, 0x00, 0x00, 0x00}).empty());
    }

    SUBCASE("a short type-1 bounds field reads by its own length")
    {
        // length == 4 means the formatted area is the header alone, so every field
        // this SDK wants is out of bounds. Bounding by the buffer instead would
        // read string indices out of the string pool and resolve garbage.
        const std::vector<unsigned char> table{
            0x01, 0x04, 0x00, 0x00, 'A', 'C', 'M', 'E', 0x00, 0x00};
        const auto params = fp::parse_smbios_params(table);
        REQUIRE(params.size() == 3);
        for (const auto& param : params) {
            INFO("parameter: " << param.first);
            CHECK(param.second.empty());
        }
    }

    SUBCASE("an unterminated string table does not run away")
    {
        const std::vector<unsigned char> table{0x02, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 'A', 'B'};
        CHECK_NOTHROW((void)fp::parse_smbios_params(table));
    }
}

TEST_CASE("parse_smbios_params never crashes on arbitrary bytes")
{
    // A deterministic sweep rather than a real fuzzer: enough to give the ASan and
    // UBSan jobs something meaningful to instrument on the one parser here whose
    // input is entirely outside this SDK's control.
    std::uint32_t state = 0x9E3779B9U;
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };

    for (int iteration = 0; iteration != 20000; ++iteration) {
        std::vector<unsigned char> table(next() % 192U);
        for (auto& byte : table) {
            byte = static_cast<unsigned char>(next() & 0xFFU);
        }
        // Bias towards plausible headers, so the walk gets past its first guard
        // often enough to exercise the string-table logic.
        if (table.size() >= 4 && (next() & 1U) != 0U) {
            table[0] = static_cast<unsigned char>((next() % 4U) + 1U);
            table[1] = static_cast<unsigned char>(4U + (next() % 32U));
        }

        CHECK_NOTHROW((void)fp::parse_smbios_params(table));
    }
}

TEST_CASE("the resolver reads identity once and shares it across threads")
{
    std::atomic<int> reads{0};

    moonbase::moonbase_device_id_resolver_options options;
    options.platform = "linux";
    options.reader = [&reads]() -> moonbase::device_identity {
        reads.fetch_add(1);
        return {{{"machineId", "b08dfa6083e7567a1921a715000001fb"}}, "host-1"};
    };
    const moonbase::moonbase_device_id_resolver resolver(options);

    constexpr int thread_count = 8;
    std::vector<std::string> ids(thread_count);
    std::vector<std::string> names(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int index = 0; index != thread_count; ++index) {
        threads.emplace_back([&, index] {
            ids[static_cast<std::size_t>(index)] = resolver.device_id();
            names[static_cast<std::size_t>(index)] = resolver.device_name();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // Reading per call would double the cost of every activation request and let
    // the name and the id come from two different reads of the machine.
    CHECK(reads.load() == 1);
    for (int index = 0; index != thread_count; ++index) {
        CHECK(ids[static_cast<std::size_t>(index)] == ids[0]);
        CHECK(names[static_cast<std::size_t>(index)] == "host-1");
    }
    CHECK(ids[0].rfind("mbd2_", 0) == 0);
}

TEST_CASE("a failed identity read is retried, not cached")
{
    std::atomic<int> attempts{0};

    moonbase::moonbase_device_id_resolver_options options;
    options.platform = "linux";
    options.reader = [&attempts]() -> moonbase::device_identity {
        // Nothing identifying on the first call, a real id afterwards. A sticky
        // failure would outlive the condition that caused it.
        if (attempts.fetch_add(1) == 0) {
            return {{{"sysVendor", "LENOVO"}}, "host-1"};
        }
        return {{{"machineId", "b08dfa6083e7567a1921a715000001fb"}}, "host-1"};
    };
    const moonbase::moonbase_device_id_resolver resolver(options);

    CHECK_THROWS_AS(resolver.device_id(), moonbase::insufficient_device_identity_error);
    // The identity read itself is memoized, so the retry sees the same params and
    // fails the same way. What must not happen is the exception being replaced by
    // a cached success or a different error.
    CHECK_THROWS_AS(resolver.device_id(), moonbase::insufficient_device_identity_error);
}

TEST_CASE("device_name survives a machine with no identity")
{
    moonbase::moonbase_device_id_resolver_options options;
    options.platform = "unknown";
    options.reader = []() -> moonbase::device_identity { return {{}, "PC-1"}; };
    const moonbase::moonbase_device_id_resolver resolver(options);

    // Activation sends the name alongside the id, and support needs a label even
    // for a machine that cannot be fingerprinted.
    CHECK(resolver.device_name() == "PC-1");
    CHECK_THROWS_AS(resolver.device_id(), moonbase::insufficient_device_identity_error);
}

TEST_CASE("the host-name fallback is opt-in and separately stamped")
{
    moonbase::moonbase_device_id_resolver_options options;
    options.platform = "unknown";
    options.fallback = moonbase::device_id_fallback::device_name;
    options.reader = []() -> moonbase::device_identity { return {{}, "PC-1"}; };
    const moonbase::moonbase_device_id_resolver resolver(options);

    // Matches the spec's worked example, including the real platform tag in the
    // material and the `n` in the stamp so the weaker binding is visible.
    CHECK(resolver.device_id()
        == "mbd2n_493978eb157552e60a13694bd6861b2a82d0dba2746431a5f7921951ed460045");

    const auto described = resolver.describe_device();
    REQUIRE(described.has_value());
    CHECK(described->source == fp::device_id_source::device_name);
    CHECK(described->param_names == std::vector<std::string>{"deviceName"});

    SUBCASE("an empty host name is still insufficient identity")
    {
        moonbase::moonbase_device_id_resolver_options empty_name = options;
        empty_name.reader = []() -> moonbase::device_identity { return {{}, ""}; };
        const moonbase::moonbase_device_id_resolver weak(empty_name);
        CHECK_THROWS_AS(weak.device_id(), moonbase::insufficient_device_identity_error);
    }
}

TEST_CASE("Mac Catalyst takes the macOS hardware identity, not the scoped one")
{
    // TARGET_OS_IPHONE is 1 for Mac Catalyst, so the obvious test picks the scoped
    // iOS path and emits an ios/mbd2s_ id. A Catalyst app runs on macOS and can
    // read IOKit, so it must agree with an Electron or web SDK on the same Mac.
#if defined(__APPLE__)
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
    CHECK(fp::platform_tag() == "mac");
#else
    CHECK(fp::platform_tag() == "ios");
#endif

    // Whichever branch this build took, the tag must be one the spec defines and
    // must match what the resolver's reader was compiled for.
    const auto tag = fp::platform_tag();
    CHECK((tag == "mac" || tag == "ios"));
#if defined(MOONBASE_FINGERPRINT_USE_IOKIT)
    // IOKit compiled in implies hardware identity, which implies the mac tag.
    CHECK(tag == "mac");
#endif
#endif
}

TEST_CASE("the host-name fallback is refused on iOS and Android")
{
    // Not merely discouraged: forbidden. Since iOS 17 gethostname() returns the
    // literal "localhost" on every device and UIDevice.name returns the model name,
    // so an SDK that fell through here when identifierForVendor was momentarily
    // absent would hand its entire install base one device id, and one activation
    // would unlock every device. Absence is transient; the error is the answer.
    for (const char* platform : {"ios", "android"}) {
        INFO("platform: " << platform);

        moonbase::moonbase_device_id_resolver_options options;
        options.platform = platform;
        options.fallback = moonbase::device_id_fallback::device_name;
        options.reader = []() -> moonbase::device_identity { return {{}, "localhost"}; };
        const moonbase::moonbase_device_id_resolver resolver(options);

        CHECK_THROWS_AS(resolver.device_id(), moonbase::insufficient_device_identity_error);
    }

    SUBCASE("but still honoured where the host name means something")
    {
        moonbase::moonbase_device_id_resolver_options options;
        options.platform = "unknown";
        options.fallback = moonbase::device_id_fallback::device_name;
        options.reader = []() -> moonbase::device_identity { return {{}, "PC-1"}; };
        const moonbase::moonbase_device_id_resolver resolver(options);
        CHECK(resolver.device_id().rfind("mbd2n_", 0) == 0);
    }
}

TEST_CASE("androidId must look like a real SSAID")
{
    // The rule that makes the JUCE defect mechanically impossible rather than
    // merely documented: reading the Settings.Secure.ANDROID_ID static field yields
    // the key name "android_id", identical on every device, and it is not hex.
    CHECK(!fp::is_valid_android_id("android_id"));
    CHECK(!fp::is_valid_android_id("A1B2C3D4E5F60718")); // uppercase
    CHECK(!fp::is_valid_android_id("a1b2c3d4e5f607189")); // 17 chars
    CHECK(!fp::is_valid_android_id(""));
    CHECK(!fp::is_valid_android_id("9774d56d682e549c")); // real, but shared by many devices
    CHECK(!fp::is_valid_android_id("0000000000000000"));

    CHECK(fp::is_valid_android_id("a1b2c3d4e5f60718"));
    // 1..16, not exactly 16: AOSP before 8.0 used Long.toHexString, which drops
    // leading zeros, so a strict 16 would reject roughly one in sixteen pre-Oreo
    // devices.
    CHECK(fp::is_valid_android_id("1b2c3d4e5f60718"));
    CHECK(fp::is_valid_android_id("a"));

    SUBCASE("the shared sentinel is rejected for androidId only")
    {
        // It must NOT join the global placeholder list: that applies to every
        // identifying parameter, so a Windows baseboard serial reading exactly this
        // string would start producing a different device id and invalidate an
        // existing binding without a spec version bump.
        const auto kept = fp::canonicalize_params(
            {{"systemManufacturer", "ACME"}, {"baseboardSerialNumber", "9774d56d682e549c"}});
        REQUIRE(kept.size() == 2);
        CHECK(kept[1].second == "9774d56d682e549c");
        CHECK(!fp::is_not_programmed("9774d56d682e549c"));
    }
}

TEST_CASE("the mobile fallback ban is enforced at the material, not just the resolver")
{
    // In build_fingerprint_material so it also binds a custom reader and a native
    // bridge assembling material directly, not only moonbase_device_id_resolver.
    for (const char* platform : {"ios", "android"}) {
        INFO("platform: " << platform);
        CHECK_THROWS_AS(
            fp::build_fingerprint_material(platform, {{"deviceName", "localhost"}}),
            moonbase::insufficient_device_identity_error);
    }

    // Everywhere else the fallback is still a legitimate opt-in.
    CHECK_NOTHROW((void)fp::build_fingerprint_material("unknown", {{"deviceName", "PC-1"}}));
}

TEST_CASE("describe_device reports names only, and hands out a copy")
{
    moonbase::moonbase_device_id_resolver_options options;
    options.platform = "linux";
    options.reader = []() -> moonbase::device_identity {
        return {{{"machineId", "b08dfa6083e7567a1921a715000001fb"},
                 {"sysVendor", "LENOVO"},
                 {"productName", ""}},
                "host-1"};
    };
    const moonbase::moonbase_device_id_resolver resolver(options);

    auto described = resolver.describe_device().value();
    CHECK(described.version == 2);
    CHECK(described.platform == "linux");
    CHECK(described.source == fp::device_id_source::identity);
    // Dropped parameters are absent, and no value ever appears: these are hardware
    // serials, and an unsalted digest of one is a stable global correlator.
    CHECK(described.param_names == std::vector<std::string>{"machineId", "sysVendor"});

    const auto id_before = resolver.device_id();
    described.device_id = "tampered";
    described.param_names.clear();
    CHECK(resolver.device_id() == id_before);
    CHECK(resolver.describe_device()->param_names.size() == 2);
}

TEST_CASE("the host resolver either produces a stamped id or refuses")
{
    // Tolerant on purpose: a container with no DMI and no machine-id legitimately
    // has no device identity. What must never happen is a third outcome, such as
    // a bare digest, an unstamped id, or a silent host-name substitution.
    const moonbase::moonbase_device_id_resolver resolver;

    try {
        const auto id = resolver.device_id();
        INFO("device id: " << id);
        CHECK(id.size() == 69);
        CHECK(id.rfind("mbd2_", 0) == 0);

        const auto parsed = fp::parse_device_id_stamp(id);
        REQUIRE(parsed.has_value());
        CHECK(parsed->version == fp::version);
        CHECK(parsed->source == fp::device_id_source::identity);
    } catch (const moonbase::insufficient_device_identity_error& ex) {
        INFO("no hardware identity on this host: " << ex.what());
        CHECK(ex.type() == moonbase::error_type::device_identity_unavailable);
    }
}

#if defined(_WIN32)
TEST_CASE("Windows reads a real SMBIOS table")
{
    // Not tolerant, deliberately. Before spec adoption this SDK built the RSMB
    // provider signature byte-reversed, so GetSystemFirmwareTable returned 0 on
    // every machine and every Windows device id silently degraded to a host-name
    // hash. Nothing in the conformance vectors can catch that; only asserting on
    // a real Windows host can.
    const auto identity = moonbase::moonbase_device_id_resolver::read_host_identity();
    CHECK(!identity.params.empty());

    const auto kept = fp::canonicalize_params(identity.params);
    const bool identifies = std::any_of(kept.begin(), kept.end(), [](const fp::parameter& param) {
        return fp::is_identifying_param(param.first);
    });
    INFO("collected " << identity.params.size() << " parameters, " << kept.size() << " survived");
    CHECK(identifies);
}
#endif
