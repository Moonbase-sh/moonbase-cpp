// Licenses that cover only some versions of the app: the shared conformance
// vectors, reading the l:rel:* claims, and how the validator, the client, the
// licensing facade and the inventory client treat a version a license doesn't
// cover.
//
// tests/vectors/release-access-vectors.json is vendored verbatim from
// @moonbase.sh/licensing. Moonbase and every SDK must agree on every case.

#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/detail/time.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/inventory.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/release_access.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

#ifndef MOONBASE_RELEASE_ACCESS_VECTORS_PATH
#error "MOONBASE_RELEASE_ACCESS_VECTORS_PATH must be defined (see CMakeLists.txt)"
#endif

using namespace moonbase;

namespace {

const nlohmann::json& vectors()
{
    static const nlohmann::json loaded = [] {
        std::ifstream file(MOONBASE_RELEASE_ACCESS_VECTORS_PATH);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " MOONBASE_RELEASE_ACCESS_VECTORS_PATH);
        nlohmann::json json;
        file >> json;
        return json;
    }();
    return loaded;
}

std::optional<std::chrono::system_clock::time_point> vector_time(const nlohmann::json& value)
{
    if (value.is_null()) {
        return std::nullopt;
    }
    return detail::parse_iso8601_utc(value.get<std::string>());
}

licensing_options options_for(const std::string& public_key, std::optional<std::string> app_version)
{
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.public_key = public_key;
    options.account_id = "tenant-1";
    options.target_platform = platform::unknown;
    options.application_version = std::move(app_version);
    return options;
}

license_validator make_validator(
    const std::string& public_key,
    std::optional<std::string> app_version = std::nullopt,
    bool enforce = true)
{
    auto options = options_for(public_key, std::move(app_version));
    options.enforce_release_access = enforce;
    return license_validator(options, std::make_shared<static_device_id_resolver>("device-name", "device-id"));
}

// Up to 2.x, the shape of a "versions up to a major" license.
nlohmann::json up_to_major_two()
{
    auto claims = tests::default_claims();
    claims["l:rel:major"] = 2;
    return claims;
}

// Updates ended a year ago, after 2.3.0 came out.
nlohmann::json updates_ended()
{
    auto claims = tests::default_claims();
    claims["l:rel:until"] = tests::now_seconds() - 365LL * 24 * 3600;
    claims["l:rel:upto"] = "2.3.0";
    return claims;
}

struct facade_fixture {
    tests::generated_key key = tests::generate_key();
    std::shared_ptr<static_device_id_resolver> device_ids =
        std::make_shared<static_device_id_resolver>("Test Device", "device-id");
    std::shared_ptr<tests::recording_transport> transport = std::make_shared<tests::recording_transport>();
    std::optional<licensing> instance;

    explicit facade_fixture(std::string app_version = "3.0.0")
    {
        auto options = options_for(key.public_pem, std::move(app_version));
        instance.emplace(options, nullptr, device_ids, transport);
    }

    std::string token(nlohmann::json claims) const { return tests::make_token(key.key.get(), std::move(claims)); }
};

std::string not_covered_problem(const std::string& fresh_token)
{
    nlohmann::json problem{
        {"title", "Version not covered"},
        {"status", 400},
        {"detail", "The license doesn't cover version 3.0.0"},
        {"errorType", "ReleaseNotCovered"},
        {"appVersion", "3.0.0"},
        {"upgradeUrl", "https://demo.moonbase.sh/buy/demo-app?variation=v1&license=license-123"},
        {"releaseAccess", {{"updatesUntil", "2026-06-01T00:00:00Z"}, {"upToVersion", "2.3.0"}}},
    };
    if (!fresh_token.empty()) {
        problem["license"] = fresh_token;
    }
    return problem.dump();
}

} // namespace

TEST_CASE("release access vectors: covers_version agrees with every case")
{
    REQUIRE(vectors().at("version").get<int>() == 1);
    const auto& cases = vectors().at("cases");
    REQUIRE_FALSE(cases.empty());

    for (const auto& item : cases) {
        CAPTURE(item.at("description").get<std::string>());
        license_release_access access;
        access.updates_until = vector_time(item.at("updatesUntil"));
        if (!item.at("upToMajor").is_null()) {
            access.up_to_major = item.at("upToMajor").get<long long>();
        }
        if (!item.at("upToVersion").is_null()) {
            access.up_to_version = item.at("upToVersion").get<std::string>();
        }

        const auto now = *vector_time(item.at("now"));
        CHECK(covers_version(access, item.at("version").get<std::string>(), now)
              == item.at("covered").get<bool>());
    }
}

TEST_CASE("release access vectors: the token claims read into the same answer")
{
    auto key = tests::generate_key();
    const auto validator = make_validator(key.public_pem);

    for (const auto& item : vectors().at("cases")) {
        CAPTURE(item.at("description").get<std::string>());
        auto claims = tests::default_claims();
        if (const auto until = vector_time(item.at("updatesUntil"))) {
            claims["l:rel:until"] = detail::to_epoch_seconds(*until);
        }
        if (!item.at("upToMajor").is_null()) {
            claims["l:rel:major"] = item.at("upToMajor");
        }
        if (!item.at("upToVersion").is_null()) {
            claims["l:rel:upto"] = item.at("upToVersion");
        }

        const auto parsed = validator.validate_token(tests::make_token(key.key.get(), claims));
        const auto now = *vector_time(item.at("now"));
        CHECK(covers_version(parsed, item.at("version").get<std::string>(), now)
              == item.at("covered").get<bool>());
    }
}

TEST_CASE("parse_app_version reads a version the way Moonbase does")
{
    const auto read = [](const char* text) { return parse_app_version(text); };

    REQUIRE(read("2.4.1"));
    CHECK(read("2.4.1")->major == 2);
    CHECK(read("2.4.1")->minor == 4);
    CHECK(read("2.4.1")->patch == 1);

    CHECK(compare(*read("v3.0.0-beta.2"), *read("3.0.0")) == 0);
    CHECK(read("v3.0.0-beta.2")->prerelease.empty());
    CHECK(compare(*read("2.4.1.0"), *read("2.4.1")) == 0);
    CHECK(compare(*read("3.1"), *read("3.1.0")) == 0);
    CHECK(compare(*read(" V2.0 "), *read("2.0.0")) == 0);
    CHECK(compare(*read("1.2.3+build.5"), *read("1.2.3")) == 0);
    CHECK(compare(*read("1.2.3 (build 77)"), *read("1.2.3")) == 0);
    CHECK(compare(*read("007.0"), *read("7.0.0")) == 0);
    CHECK(read("9999999.0"));

    CHECK_FALSE(read(""));
    CHECK_FALSE(read("   "));
    CHECK_FALSE(read("nightly"));
    CHECK_FALSE(read("3"));
    CHECK_FALSE(read("1.2.3.4.5"));
    CHECK_FALSE(read("1..2"));
    CHECK_FALSE(read("1.x"));
    CHECK_FALSE(read("-1.0"));
    CHECK_FALSE(read("10000000.0"));
}

TEST_CASE("a license without release access claims covers every version")
{
    auto key = tests::generate_key();
    const auto parsed = make_validator(key.public_pem).validate_token(
        tests::make_token(key.key.get(), tests::default_claims()));

    CHECK_FALSE(parsed.release_access.has_value());
    CHECK(covers_version(parsed, "99.0.0"));
}

TEST_CASE("release access claims are read from numbers or strings holding them")
{
    auto key = tests::generate_key();
    auto claims = tests::default_claims();
    claims["l:rel:until"] = "1780272000"; // 2026-06-01T00:00:00Z
    claims["l:rel:major"] = "2";
    claims["l:rel:upto"] = "2.3.0";

    const auto parsed = make_validator(key.public_pem).validate_token(tests::make_token(key.key.get(), claims));

    REQUIRE(parsed.release_access.has_value());
    REQUIRE(parsed.release_access->updates_until.has_value());
    CHECK(detail::format_iso8601_utc(*parsed.release_access->updates_until) == "2026-06-01T00:00:00Z");
    CHECK(parsed.release_access->up_to_major == 2);
    CHECK(parsed.release_access->up_to_version == "2.3.0");
}

TEST_CASE("a release access claim that can't be read is left out, not a failed license")
{
    auto key = tests::generate_key();
    const auto validator = make_validator(key.public_pem);
    const auto parse = [&](nlohmann::json claims) {
        return validator.validate_token(tests::make_token(key.key.get(), std::move(claims)));
    };

    auto claims = tests::default_claims();
    claims["l:rel:until"] = "soon";
    claims["l:rel:major"] = 2.5;
    claims["l:rel:upto"] = "2.3.0";
    // Neither limit can be read, so nothing is limited, and upto alone means nothing.
    CHECK_FALSE(parse(claims).release_access.has_value());

    claims["l:rel:major"] = -1;
    CHECK_FALSE(parse(claims).release_access.has_value());

    claims["l:rel:major"] = true;
    CHECK_FALSE(parse(claims).release_access.has_value());

    claims["l:rel:major"] = nullptr;
    claims["l:rel:until"] = "soon";
    CHECK_FALSE(parse(claims).release_access.has_value());

    claims = up_to_major_two();
    claims["l:rel:upto"] = "the latest";
    const auto limited = parse(claims);
    REQUIRE(limited.release_access.has_value());
    CHECK(limited.release_access->up_to_major == 2);
    CHECK_FALSE(limited.release_access->up_to_version.has_value());
}

TEST_CASE("release access survives the store's JSON round trip")
{
    auto key = tests::generate_key();
    const auto parsed = make_validator(key.public_pem).validate_token(
        tests::make_token(key.key.get(), updates_ended()));
    REQUIRE(parsed.release_access.has_value());

    const auto restored = nlohmann::json(parsed).get<license>();
    REQUIRE(restored.release_access.has_value());
    CHECK(restored.release_access->updates_until == parsed.release_access->updates_until);
    CHECK_FALSE(restored.release_access->up_to_major.has_value());
    CHECK(restored.release_access->up_to_version == "2.3.0");

    const auto unlimited = nlohmann::json(make_validator(key.public_pem).validate_token(
        tests::make_token(key.key.get(), tests::default_claims())));
    CHECK_FALSE(unlimited.contains("releaseAccess"));
    CHECK_FALSE(unlimited.get<license>().release_access.has_value());
}

TEST_CASE("a stored release access field that can't be read costs nothing but itself")
{
    auto key = tests::generate_key();
    auto json = nlohmann::json(make_validator(key.public_pem).validate_token(
        tests::make_token(key.key.get(), up_to_major_two())));
    json["releaseAccess"]["updatesUntil"] = "+294247-01-10T04:00:54Z";
    json["releaseAccess"]["upToVersion"] = 5;

    const auto restored = json.get<license>();
    REQUIRE(restored.release_access.has_value());
    CHECK(restored.release_access->up_to_major == 2);
    CHECK_FALSE(restored.release_access->updates_until.has_value());
    CHECK_FALSE(restored.release_access->up_to_version.has_value());

}

TEST_CASE("an end of updates past what the clock holds is held to its limit, not dropped")
{
    auto key = tests::generate_key();
    const auto parse = [&](nlohmann::json claims) {
        return make_validator(key.public_pem).validate_token(tests::make_token(key.key.get(), std::move(claims)));
    };

    // Dropping it would leave the major limit alone and refuse 3.0.0, which
    // Moonbase covers while updates last.
    auto claims = up_to_major_two();
    claims["l:rel:until"] = 10413792000LL; // 2300-01-01, past where some clocks end
    const auto distant = parse(claims);
    REQUIRE(distant.release_access.has_value());
    REQUIRE(distant.release_access->updates_until.has_value());
    CHECK(covers_version(distant, "3.0.0"));

    claims["l:rel:until"] = 9223372036854775807LL;
    CHECK(covers_version(parse(claims), "3.0.0"));

    // The other way, the end has long passed.
    claims["l:rel:until"] = -9223372036854775807LL;
    claims["l:rel:upto"] = "2.3.0";
    const auto ancient = parse(claims);
    REQUIRE(ancient.release_access.has_value());
    CHECK_FALSE(covers_version(ancient, "3.0.0"));
    CHECK(covers_version(ancient, "2.0.0"));

    // Still a date the store can write and read back.
    const auto restored = nlohmann::json(distant).get<license>();
    REQUIRE(restored.release_access.has_value());
    CHECK(restored.release_access->updates_until.has_value());
}

TEST_CASE("the validator refuses a version the license doesn't cover")
{
    auto key = tests::generate_key();
    const auto token = tests::make_token(key.key.get(), up_to_major_two());

    try {
        (void)make_validator(key.public_pem, "v3.0.0-beta.2").validate_token(token);
        FAIL("expected the version to be refused");
    } catch (const release_not_covered_error& ex) {
        CHECK(ex.type() == error_type::release_not_covered);
        CHECK(std::string(ex.what()) == "The license doesn't cover version 3.0.0");
        CHECK(ex.app_version() == "3.0.0");
        REQUIRE(ex.license().has_value());
        CHECK(ex.license()->id == "license-123");
        REQUIRE(ex.release_access().has_value());
        CHECK(ex.release_access()->up_to_major == 2);
        // Only Moonbase knows where to buy the upgrade.
        CHECK_FALSE(ex.upgrade_url().has_value());
        // So an existing catch keeps refusing to run, and the grace period can't relax it.
        CHECK(dynamic_cast<const license_invalid_error*>(&ex) != nullptr);
    }

    CHECK_NOTHROW((void)make_validator(key.public_pem, "2.9.9").validate_token(token));
    CHECK_NOTHROW((void)make_validator(key.public_pem, "nightly").validate_token(token));
    CHECK_NOTHROW((void)make_validator(key.public_pem).validate_token(token));
    CHECK_NOTHROW((void)make_validator(key.public_pem, "3.0.0", /*enforce*/ false).validate_token(token));

    const auto validator = make_validator(key.public_pem, "3.0.0");
    CHECK_NOTHROW((void)validator.validate_token_ignoring_release_access(token));
    CHECK_NOTHROW((void)validator.validate_token_allow_expired(token));
    CHECK_THROWS_AS(
        validator.ensure_covers_application_version(validator.validate_token_ignoring_release_access(token)),
        release_not_covered_error);
}

TEST_CASE("updates that last cover every version, and end on the device clock")
{
    auto key = tests::generate_key();
    auto claims = tests::default_claims();
    claims["l:rel:until"] = tests::now_seconds() + 3600;
    CHECK_NOTHROW((void)make_validator(key.public_pem, "9.0.0").validate_token(
        tests::make_token(key.key.get(), claims)));

    CHECK_THROWS_AS(
        (void)make_validator(key.public_pem, "2.3.1").validate_token(tests::make_token(key.key.get(), updates_ended())),
        release_not_covered_error);
    CHECK_NOTHROW((void)make_validator(key.public_pem, "2.3.0").validate_token(
        tests::make_token(key.key.get(), updates_ended())));
}

TEST_CASE("validate_token_online reads ReleaseNotCovered with the license Moonbase issued along")
{
    facade_fixture fixture;
    auto stale = updates_ended();
    stale["validated"] = tests::now_seconds() - 10 * 60; // past the 5 minute throttle
    const auto stale_token = fixture.token(stale);

    auto fresh = updates_ended();
    fresh["validated"] = tests::now_seconds();
    const auto fresh_token = fixture.token(fresh);
    fixture.transport->responses.push_back(http_response{400, {}, not_covered_problem(fresh_token)});

    try {
        (void)fixture.instance->validate_token_online(stale_token);
        FAIL("expected the version to be refused");
    } catch (const release_not_covered_error& ex) {
        CHECK(std::string(ex.what()) == "The license doesn't cover version 3.0.0");
        CHECK(ex.app_version() == "3.0.0");
        CHECK(ex.upgrade_url() == "https://demo.moonbase.sh/buy/demo-app?variation=v1&license=license-123");
        REQUIRE(ex.release_access().has_value());
        CHECK(detail::format_iso8601_utc(*ex.release_access()->updates_until) == "2026-06-01T00:00:00Z");
        CHECK(ex.release_access()->up_to_version == "2.3.0");
        REQUIRE(ex.license().has_value());
        CHECK(ex.license()->token == fresh_token);
    }

    // Kept, not deleted: it still covers the versions it did.
    const auto stored = fixture.instance->store().load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == fresh_token);
    REQUIRE(fixture.transport->requests.size() == 1);
}

TEST_CASE("convert_legacy_license stores the license Moonbase sends with ReleaseNotCovered")
{
    facade_fixture fixture;
    auto fresh = updates_ended();
    fresh["validated"] = tests::now_seconds();
    const auto fresh_token = fixture.token(fresh);
    fixture.transport->responses.push_back(http_response{400, {}, not_covered_problem(fresh_token)});

    try {
        (void)fixture.instance->convert_legacy_license("legacy");
        FAIL("expected the version to be refused");
    } catch (const release_not_covered_error& ex) {
        REQUIRE(ex.license().has_value());
        CHECK(ex.license()->token == fresh_token);
    }

    // Kept: it covers the versions it did, so the next launch starts from it.
    const auto stored = fixture.instance->store().load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == fresh_token);
}

TEST_CASE("validate_token_online leaves the store alone on ReleaseNotCovered when told not to persist")
{
    facade_fixture fixture;
    auto stale = updates_ended();
    stale["validated"] = tests::now_seconds() - 10 * 60;
    auto fresh = updates_ended();
    fresh["validated"] = tests::now_seconds();
    fixture.transport->responses.push_back(http_response{400, {}, not_covered_problem(fixture.token(fresh))});

    CHECK_THROWS_AS(
        (void)fixture.instance->validate_token_online(fixture.token(stale), [] { return false; }),
        release_not_covered_error);
    CHECK_FALSE(fixture.instance->store().load_local_license().has_value());
}

TEST_CASE("Moonbase judges an online license, so an upgrade bought since the token was issued counts")
{
    facade_fixture fixture;
    auto stale = updates_ended(); // doesn't cover 3.0.0 on the device
    stale["validated"] = tests::now_seconds() - 10 * 60;

    auto upgraded = tests::default_claims();
    upgraded["l:rel:until"] = tests::now_seconds() + 365LL * 24 * 3600;
    upgraded["validated"] = tests::now_seconds();
    const auto upgraded_token = fixture.token(upgraded);
    fixture.transport->responses.push_back(http_response{200, {}, upgraded_token});

    const auto result = fixture.instance->validate_token_online(fixture.token(stale));

    CHECK(result.token == upgraded_token);
    CHECK(fixture.transport->requests.size() == 1);
}

TEST_CASE("the device judges the version when Moonbase isn't asked")
{
    SUBCASE("within the min interval")
    {
        facade_fixture fixture;
        CHECK_THROWS_AS(
            (void)fixture.instance->validate_token_online(fixture.token(updates_ended())),
            release_not_covered_error);
        CHECK(fixture.transport->requests.empty());
    }

    SUBCASE("unreachable within the grace period")
    {
        facade_fixture fixture;
        auto stale = updates_ended();
        stale["validated"] = tests::now_seconds() - 10 * 60;
        // No queued response: the transport throws, as an unreachable server does.
        CHECK_THROWS_AS(
            (void)fixture.instance->validate_token_online(fixture.token(stale)),
            release_not_covered_error);
        CHECK(fixture.transport->requests.size() == 1);
    }

    SUBCASE("an offline license")
    {
        facade_fixture fixture;
        auto offline = up_to_major_two();
        offline["method"] = "Offline";
        const auto token = fixture.token(offline);
        CHECK_THROWS_AS((void)fixture.instance->validate_token_online(token), release_not_covered_error);
        CHECK_THROWS_AS((void)fixture.instance->read_offline_license(token), release_not_covered_error);
        CHECK(fixture.transport->requests.empty());

        facade_fixture covered("2.4.0");
        CHECK_NOTHROW((void)covered.instance->read_offline_license(covered.token(offline)));

        // An online token passed off as a license file is refused as such, not
        // handed back to be stored because it doesn't cover the version.
        CHECK_THROWS_WITH_AS(
            (void)fixture.instance->read_offline_license(fixture.token(up_to_major_two())),
            "License token is not an offline-activated license",
            license_invalid_error);
    }
}

TEST_CASE("an activation Moonbase fulfilled is handed over, whatever the device makes of the version")
{
    facade_fixture fixture;
    fixture.transport->responses.push_back(http_response{200, {}, fixture.token(up_to_major_two())});

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    const auto activated = fixture.instance->get_requested_activation(request);

    // A seat is taken by now, so dropping the license here would strand it.
    REQUIRE(activated.has_value());
    CHECK(activated->release_access.has_value());
}

TEST_CASE("a license that doesn't cover this version can still be deactivated")
{
    facade_fixture fixture;
    fixture.transport->responses.push_back(http_response{200, {}, ""});

    CHECK_NOTHROW(fixture.instance->revoke_activation(fixture.token(up_to_major_two())));
    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(fixture.transport->requests.front().url.find("/revoke") != std::string::npos);
}

TEST_CASE("add-ons are managed on a license whatever version runs")
{
    facade_fixture fixture;
    const auto token = fixture.token(up_to_major_two()); // doesn't cover 3.0.0
    fixture.transport->responses.push_back(http_response{200, {}, token});

    // The seat is taken by the time the answer comes, so it is handed over and
    // stored rather than refused for the version.
    const auto result = fixture.instance->activate_sub_product(token, "drum-kit");
    CHECK(result.token == token);
    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(fixture.transport->requests.front().url.find("/sub-products/drum-kit/activate") != std::string::npos);
    const auto stored = fixture.instance->store().load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == token);
}

TEST_CASE("a ReleaseNotCovered problem is read leniently")
{
    facade_fixture fixture;
    auto stale = up_to_major_two();
    stale["validated"] = tests::now_seconds() - 10 * 60;

    SUBCASE("a license in it that fails verification is left out")
    {
        auto other = tests::generate_key();
        fixture.transport->responses.push_back(http_response{
            400, {}, not_covered_problem(tests::make_token(other.key.get(), up_to_major_two()))});
        try {
            (void)fixture.instance->validate_token_online(fixture.token(stale));
            FAIL("expected the version to be refused");
        } catch (const release_not_covered_error& ex) {
            CHECK_FALSE(ex.license().has_value());
            CHECK(ex.upgrade_url().has_value());
        }
        CHECK_FALSE(fixture.instance->store().load_local_license().has_value());
    }

    SUBCASE("extensions of the wrong shape still refuse")
    {
        fixture.transport->responses.push_back(http_response{
            400, {}, R"({"errorType":"ReleaseNotCovered","releaseAccess":"2.x","upgradeUrl":5,"appVersion":null})"});
        try {
            (void)fixture.instance->validate_token_online(fixture.token(stale));
            FAIL("expected the version to be refused");
        } catch (const release_not_covered_error& ex) {
            CHECK(std::string(ex.what()) == "The license doesn't cover version 3.0.0");
            CHECK(ex.app_version() == "3.0.0");
            CHECK_FALSE(ex.upgrade_url().has_value());
            CHECK_FALSE(ex.release_access().has_value());
        }
    }

    SUBCASE("a date without an offset is UTC")
    {
        fixture.transport->responses.push_back(http_response{
            400, {}, R"({"errorType":"ReleaseNotCovered","releaseAccess":{"updatesUntil":"2026-06-01T00:00:00.000"}})"});
        try {
            (void)fixture.instance->validate_token_online(fixture.token(stale));
            FAIL("expected the version to be refused");
        } catch (const release_not_covered_error& ex) {
            REQUIRE(ex.release_access().has_value());
            CHECK(detail::format_iso8601_utc(*ex.release_access()->updates_until) == "2026-06-01T00:00:00Z");
        }
    }
}

TEST_CASE("get_release says when the customer's licenses don't cover the release")
{
    auto transport = std::make_shared<tests::recording_transport>(std::deque<http_response>{
        http_response{200, {},
            R"({"id":"demo-app","name":"Solstice","version":"3.0.0","downloadsNeedsOwnership":true,)"
            R"("downloads":[{"name":"Solstice-3.0.0.dmg","platform":"Mac"}],)"
            R"("releaseAccess":{"upToMajor":2,"coversRelease":false,"latestCoveredVersion":"2.9.0",)"
            R"("upgradeUrl":"https://demo.moonbase.sh/buy/demo-app?variation=v1&license=l1"}})"},
        http_response{200, {}, R"({"id":"demo-app","name":"Solstice","version":"2.9.0","downloads":[]})"},
    });
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    inventory_client client(options, transport);

    const auto info = client.get_release("3.0.0", "tok");
    CHECK_FALSE(info.release_covered);
    REQUIRE(info.release_access.has_value());
    CHECK(info.release_access->up_to_major == 2);
    CHECK(info.upgrade_url == "https://demo.moonbase.sh/buy/demo-app?variation=v1&license=l1");

    license owner;
    owner.issued_to.id = "user-123";
    CHECK_FALSE(can_download(owner, info));

    const auto unlimited = client.get_release("2.9.0", "tok");
    CHECK(unlimited.release_covered);
    CHECK_FALSE(unlimited.release_access.has_value());
    CHECK_FALSE(unlimited.upgrade_url.has_value());
}

TEST_CASE("a download the customer's licenses don't cover is refused with the upgrade link")
{
    auto transport = std::make_shared<tests::recording_transport>(std::deque<http_response>{
        http_response{403, {},
            R"({"title":"Not covered by your license","status":403,"detail":"Your license doesn't cover this version",)"
            R"("errorType":"ReleaseNotCovered","releaseAccess":{"upToMajor":2,"coversRelease":false,)"
            R"("upgradeUrl":"https://demo.moonbase.sh/buy/demo-app?variation=v1&license=l1"},)"
            R"("upgradeUrl":"https://demo.moonbase.sh/buy/demo-app?variation=v1&license=l1"})"},
    });
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    inventory_client client(options, transport);

    try {
        (void)client.get_download_url("Mac", "tok");
        FAIL("expected the download to be refused");
    } catch (const release_not_covered_error& ex) {
        CHECK(std::string(ex.what()) == "Your license doesn't cover this version");
        CHECK(ex.upgrade_url() == "https://demo.moonbase.sh/buy/demo-app?variation=v1&license=l1");
        REQUIRE(ex.release_access().has_value());
        CHECK(ex.release_access()->up_to_major == 2);
        CHECK_FALSE(ex.license().has_value());
        CHECK(ex.app_version().empty());
    }
}
