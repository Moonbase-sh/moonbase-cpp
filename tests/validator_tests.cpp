#include <doctest/doctest.h>

#include <chrono>
#include <memory>

#include "moonbase/detail/time.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

licensing_options options_for(const std::string& public_key)
{
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.public_key = public_key;
    options.account_id = "tenant-1";
    options.target_platform = platform::unknown;
    return options;
}

license_validator make_validator(const std::string& public_key, const std::string& device_id = "device-id")
{
    return license_validator(
        options_for(public_key),
        std::make_shared<static_device_id_resolver>("device-name", device_id));
}

} // namespace

TEST_CASE("valid RS256 JWT is parsed into a license")
{
    auto key = moonbase::tests::generate_key();
    auto claims = moonbase::tests::default_claims();
    const auto token = moonbase::tests::make_token(key.key.get(), claims);

    const auto result = make_validator(key.public_pem).validate_token(token);

    CHECK(result.id == "license-123");
    CHECK(result.activation_id == "activation-123");
    CHECK_FALSE(result.trial);
    CHECK(result.method == activation_method::online);
    CHECK(result.licensed_product.id == "demo-app");
    CHECK(result.licensed_product.name == "Demo Product");
    REQUIRE(result.licensed_product.current_release_version.has_value());
    CHECK(*result.licensed_product.current_release_version == "1.2.3");
    REQUIRE(result.subscription_id.has_value());
    CHECK(*result.subscription_id == "subscription-123");
    CHECK(result.owned_sub_product_ids.size() == 2);
    CHECK(result.issued_to.email == "jane@example.com");
    CHECK(result.licensed_product.properties.at("tier") == "pro");
    CHECK(result.issued_to.properties.at("company") == "Acme");
    CHECK(result.properties.at("seats") == 3);
    CHECK(result.token == token);
}

TEST_CASE("PKCS#1 RSA public keys are accepted")
{
    auto key = moonbase::tests::generate_key();
    const auto token = moonbase::tests::make_token(
        key.key.get(),
        moonbase::tests::default_claims());

    const auto result = make_validator(key.public_pkcs1_pem).validate_token(token);

    CHECK(result.id == "license-123");
}

TEST_CASE("validated timestamp can fall back to legacy ver claim")
{
    auto key = moonbase::tests::generate_key();
    auto claims = moonbase::tests::default_claims();
    claims.erase("validated");
    claims["ver"] = "2026-05-08T12:34:56.0000000Z";

    const auto result = make_validator(key.public_pem).validate_token(
        moonbase::tests::make_token(key.key.get(), claims));

    CHECK(moonbase::detail::format_iso8601_utc(result.validated_at) == "2026-05-08T12:34:56Z");
}

TEST_CASE("strict UTC timestamp parser accepts only explicit UTC timestamps")
{
    CHECK(
        moonbase::detail::format_iso8601_utc(
            moonbase::detail::parse_iso8601_utc("2026-05-08T12:34:56Z")) ==
        "2026-05-08T12:34:56Z");
    CHECK(
        moonbase::detail::format_iso8601_utc(
            moonbase::detail::parse_iso8601_utc("2026-05-08T12:34:56.1234567Z")) ==
        "2026-05-08T12:34:56Z");

    CHECK_THROWS_AS(
        (void)moonbase::detail::parse_iso8601_utc("2026-05-08T12:34:56"),
        std::runtime_error);
    CHECK_THROWS_AS(
        (void)moonbase::detail::parse_iso8601_utc("2026-05-08T12:34:56+01:00"),
        std::runtime_error);
    CHECK_THROWS_AS(
        (void)moonbase::detail::parse_iso8601_utc("2026-05-08T12:34:56Z trailing"),
        std::runtime_error);
    CHECK_THROWS_AS(
        (void)moonbase::detail::parse_iso8601_utc("2026-02-30T12:34:56Z"),
        std::runtime_error);
}

TEST_CASE("trial tokens use trial properties")
{
    auto key = moonbase::tests::generate_key();
    auto claims = moonbase::tests::default_claims();
    claims["trial"] = "true";
    claims.erase("l:properties");
    claims["t:properties"] = {{"days", 14}};

    const auto result = make_validator(key.public_pem).validate_token(
        moonbase::tests::make_token(key.key.get(), claims));

    CHECK(result.trial);
    CHECK(result.properties.at("days") == 14);
}

TEST_CASE("custom properties carry array values")
{
    // The API surface now allows array-valued custom properties (see the
    // `array` variant added to NestedPropertyValue in moonbase.js). Those
    // arrive in the JWT as plain JSON arrays inside each properties claim.
    // Lock in that arrays — including arrays of objects — round-trip through
    // validate_token for product, user, license, and trial property bags.
    auto key = moonbase::tests::generate_key();

    SUBCASE("non-trial license carries arrays in p/u/l properties")
    {
        auto claims = moonbase::tests::default_claims();
        claims["p:properties"] = {
            {"tier", "pro"},
            {"regions", {"us", "eu", "ap"}},
        };
        claims["u:properties"] = {
            {"company", "Acme"},
            {"roles", {"admin", "ops"}},
        };
        claims["l:properties"] = {
            {"seats", 3},
            {"features", {"export", "sso"}},
            {"contacts", nlohmann::json::array({
                {{"name", "Jane"}, {"email", "jane@example.com"}},
                {{"name", "John"}, {"email", "john@example.com"}},
            })},
        };

        const auto result = make_validator(key.public_pem).validate_token(
            moonbase::tests::make_token(key.key.get(), claims));

        const auto& regions = result.licensed_product.properties.at("regions");
        REQUIRE(regions.is_array());
        REQUIRE(regions.size() == 3);
        CHECK(regions.at(0) == "us");
        CHECK(regions.at(2) == "ap");

        const auto& roles = result.issued_to.properties.at("roles");
        REQUIRE(roles.is_array());
        REQUIRE(roles.size() == 2);
        CHECK(roles.at(0) == "admin");

        const auto& features = result.properties.at("features");
        REQUIRE(features.is_array());
        CHECK(features.size() == 2);

        const auto& contacts = result.properties.at("contacts");
        REQUIRE(contacts.is_array());
        REQUIRE(contacts.size() == 2);
        CHECK(contacts.at(1).at("email") == "john@example.com");
    }

    SUBCASE("trial license carries arrays in t:properties")
    {
        auto claims = moonbase::tests::default_claims();
        claims["trial"] = "true";
        claims.erase("l:properties");
        claims["t:properties"] = {
            {"days", 14},
            {"allowed_hosts", {"localhost", "demo.example.com"}},
        };

        const auto result = make_validator(key.public_pem).validate_token(
            moonbase::tests::make_token(key.key.get(), claims));

        CHECK(result.trial);
        const auto& hosts = result.properties.at("allowed_hosts");
        REQUIRE(hosts.is_array());
        REQUIRE(hosts.size() == 2);
        CHECK(hosts.at(1) == "demo.example.com");
    }
}

TEST_CASE("invalid JWTs are rejected")
{
    auto key = moonbase::tests::generate_key();
    auto other_key = moonbase::tests::generate_key();

    SUBCASE("malformed token")
    {
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token("not-a-token"), license_invalid_error);
    }

    SUBCASE("unsupported algorithm")
    {
        const auto token = moonbase::tests::make_token(
            key.key.get(),
            moonbase::tests::default_claims(),
            {{"alg", "HS256"}, {"typ", "JWT"}});
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("wrong signature")
    {
        const auto token = moonbase::tests::make_token(
            other_key.key.get(),
            moonbase::tests::default_claims());
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("wrong product audience")
    {
        auto claims = moonbase::tests::default_claims();
        claims["aud"] = "other-product";
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("wrong issuer")
    {
        auto claims = moonbase::tests::default_claims();
        claims["iss"] = "other-tenant";
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("wrong device")
    {
        auto claims = moonbase::tests::default_claims("other-device");
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("expired")
    {
        auto claims = moonbase::tests::default_claims();
        claims["exp"] = moonbase::tests::now_seconds() - 10;
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_expired_error);
    }

    SUBCASE("unknown activation method")
    {
        auto claims = moonbase::tests::default_claims();
        claims["method"] = "Sideways";
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("malformed legacy validation timestamp")
    {
        auto claims = moonbase::tests::default_claims();
        claims.erase("validated");
        claims["ver"] = "2026-02-30T12:34:56Z";
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }

    SUBCASE("malformed signature encoding")
    {
        const auto token = moonbase::tests::make_token(
            key.key.get(),
            moonbase::tests::default_claims());
        const auto signature = token.rfind('.');
        REQUIRE(signature != std::string::npos);
        const auto corrupted = token.substr(0, signature + 1) + "not@base64";
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(corrupted), license_invalid_error);
    }

    SUBCASE("missing required claim")
    {
        auto claims = moonbase::tests::default_claims();
        claims.erase("l:id");
        const auto token = moonbase::tests::make_token(key.key.get(), claims);
        CHECK_THROWS_AS((void)make_validator(key.public_pem).validate_token(token), license_invalid_error);
    }
}
