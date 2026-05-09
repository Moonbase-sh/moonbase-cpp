#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <memory>

#include <nlohmann/json.hpp>

#include "moonbase/fingerprint.hpp"
#include "moonbase/licensing.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

struct facade_fixture {
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    std::shared_ptr<static_fingerprint_provider> fingerprints =
        std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
    std::shared_ptr<moonbase::tests::recording_transport> transport =
        std::make_shared<moonbase::tests::recording_transport>();
    licensing instance;

    explicit facade_fixture(licensing_options options = {})
        : instance(prepare(std::move(options)), nullptr, fingerprints, transport)
    {
    }

    licensing_options prepare(licensing_options options)
    {
        if (options.endpoint.empty()) {
            options.endpoint = "https://demo.moonbase.sh";
        }
        if (options.product_id.empty()) {
            options.product_id = "demo-app";
        }
        if (options.public_key.empty()) {
            options.public_key = key.public_pem;
        }
        if (!options.account_id) {
            options.account_id = "tenant-1";
        }
        return options;
    }

    std::string make_token(nlohmann::json claims)
    {
        return moonbase::tests::make_token(key.key.get(), std::move(claims));
    }
};

} // namespace

TEST_CASE("validate_token_online skips API call within the min interval")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["validated"] = moonbase::tests::now_seconds() - 30; // well under default 5 min
    const auto token = fixture.make_token(claims);

    const auto result = fixture.instance.validate_token_online(token);

    CHECK(result.id == "license-123");
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("validate_token_online calls API after the min interval and returns refreshed license")
{
    facade_fixture fixture;
    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60); // 10 min > 5 min default
    const auto stale_token = fixture.make_token(stale);

    auto refreshed = moonbase::tests::default_claims();
    refreshed["validated"] = moonbase::tests::now_seconds();
    const auto refreshed_token = fixture.make_token(refreshed);
    fixture.transport->responses.push_back(http_response{200, {}, refreshed_token});

    const auto result = fixture.instance.validate_token_online(stale_token);

    CHECK(result.token == refreshed_token);
    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(fixture.transport->requests.front().url.find("/api/client/licenses/demo-app/validate") != std::string::npos);
}

TEST_CASE("validate_token_online tolerates transport failures within the grace period")
{
    facade_fixture fixture;
    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale);
    // No queued responses → recording_transport throws std::runtime_error.

    const auto result = fixture.instance.validate_token_online(stale_token);

    CHECK(result.id == "license-123");
    CHECK(result.token == stale_token);
    CHECK(fixture.transport->requests.size() == 1);
}

TEST_CASE("validate_token_online fails when transport fails and grace has elapsed")
{
    licensing_options options;
    options.online_validation_grace_period = std::chrono::seconds(60);
    options.online_validation_min_interval = std::chrono::seconds(10);
    facade_fixture fixture(options);

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (5 * 60); // 5 min > 60s grace
    const auto stale_token = fixture.make_token(stale);

    CHECK_THROWS_AS(
        (void)fixture.instance.validate_token_online(stale_token),
        std::exception);
}

TEST_CASE("validate_token_online propagates definitive license errors regardless of grace")
{
    licensing_options options;
    options.online_validation_grace_period = std::chrono::hours(24 * 365);
    facade_fixture fixture(options);

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale);

    SUBCASE("invalid")
    {
        fixture.transport->responses.push_back(
            http_response{400, {}, R"({"title":"Invalid","detail":"bad"})"});
        CHECK_THROWS_AS(
            (void)fixture.instance.validate_token_online(stale_token),
            license_invalid_error);
    }

    SUBCASE("expired")
    {
        fixture.transport->responses.push_back(
            http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"expired"})"});
        CHECK_THROWS_AS(
            (void)fixture.instance.validate_token_online(stale_token),
            license_expired_error);
    }
}

TEST_CASE("validate_token_online never contacts the API for offline-activated tokens")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["method"] = "Offline";
    claims["validated"] = moonbase::tests::now_seconds() - (24 * 60 * 60 * 365); // ancient
    const auto token = fixture.make_token(claims);

    const auto result = fixture.instance.validate_token_online(token);

    CHECK(result.method == activation_method::offline);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("validate_token_online runs local validation before any HTTP call")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims("other-device");
    claims["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto token = fixture.make_token(claims);

    CHECK_THROWS_AS(
        (void)fixture.instance.validate_token_online(token),
        license_invalid_error);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("validate_token_online honors a custom min interval")
{
    licensing_options options;
    options.online_validation_min_interval = std::chrono::seconds(1);
    facade_fixture fixture(options);

    auto claims = moonbase::tests::default_claims();
    claims["validated"] = moonbase::tests::now_seconds() - 30; // > 1s custom interval
    const auto token = fixture.make_token(claims);

    auto refreshed = moonbase::tests::default_claims();
    refreshed["validated"] = moonbase::tests::now_seconds();
    fixture.transport->responses.push_back(
        http_response{200, {}, fixture.make_token(refreshed)});

    (void)fixture.instance.validate_token_online(token);

    CHECK(fixture.transport->requests.size() == 1);
}
