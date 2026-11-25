#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/release_access.hpp"
#include "moonbase/store.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

struct migration_fixture {
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    std::shared_ptr<device_id_resolver> device_ids =
        std::make_shared<static_device_id_resolver>("Test Device", "device-id");
    std::shared_ptr<moonbase::tests::recording_transport> transport =
        std::make_shared<moonbase::tests::recording_transport>();

    [[nodiscard]] licensing_options options() const
    {
        licensing_options options;
        options.endpoint = "https://demo.moonbase.sh/";
        options.product_id = "demo-app";
        options.public_key = key.public_pem;
        options.account_id = "tenant-1";
        options.target_platform = platform::mac;
        options.application_version = "1.2.3";
        options.metadata = {{"channel", "test"}};
        return options;
    }

    [[nodiscard]] std::string token(nlohmann::json claims) const
    {
        return moonbase::tests::make_token(key.key.get(), std::move(claims));
    }

    // The license Moonbase issues for the device the validation endpoint named.
    [[nodiscard]] std::string migrated(const std::string& device_id = "device-id") const
    {
        return token(moonbase::tests::default_claims(device_id));
    }

    [[nodiscard]] license_client client() const
    {
        const auto with = options();
        return license_client(with, device_ids, std::make_shared<license_validator>(with, device_ids), transport);
    }

    void respond(http_response response) { transport->responses.push_back(std::move(response)); }

    void refuse(int status, const std::string& error_type, const std::string& detail)
    {
        nlohmann::json problem{{"title", "Refused"}, {"status", status}, {"detail", detail}};
        if (!error_type.empty()) {
            problem["errorType"] = error_type;
        }
        respond(http_response{status, {}, problem.dump()});
    }
};

std::string query_of(const std::string& url)
{
    const auto at = url.find('?');
    return at == std::string::npos ? std::string() : url.substr(at + 1);
}

} // namespace

TEST_CASE("migrate_license posts the license with this device and product, and returns the license")
{
    migration_fixture fixture;
    const auto token = fixture.migrated();
    fixture.respond(http_response{200, {{"Content-Type", "application/jwt"}}, token});

    const auto migrated = fixture.client().migrate_license("ABCD-1234-EFGH-5678");

    CHECK(migrated.token == token);
    CHECK(migrated.activation_id == "activation-123");
    CHECK(migrated.licensed_product.id == "demo-app");
    CHECK(migrated.issued_to.email == "jane@example.com");
    CHECK_FALSE(migrated.trial);

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.method == "POST");
    // Not under the product: the endpoint names it.
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/licenses/migrate?") == 0);
    const auto query = query_of(request.url);
    CHECK(query.find("format=JWT") != std::string::npos);
    CHECK(query.find("platform=Mac") != std::string::npos);
    CHECK(query.find("appVersion=1.2.3") != std::string::npos);
    CHECK(query.find("meta%5Bchannel%5D=test") != std::string::npos);
    CHECK(request.headers.at("Content-Type") == "application/json");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");

    const auto body = nlohmann::json::parse(request.body);
    CHECK(body.size() == 4);
    CHECK(body.at("license") == "ABCD-1234-EFGH-5678");
    CHECK(body.at("productId") == "demo-app");
    CHECK(body.at("deviceSignature") == "device-id");
    CHECK(body.at("deviceName") == "Test Device");
}

TEST_CASE("migrate_license sends a multi-line license file as it is")
{
    migration_fixture fixture;
    fixture.respond(http_response{200, {}, fixture.migrated()});

    // CRLF and all: the endpoint may check a signature over the exact bytes.
    const std::string file = "Jane Developer\r\n<license product=\"demo\"/>\r\nc2lnbmF0dXJl\r\n";
    (void)fixture.client().migrate_license(file);

    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(nlohmann::json::parse(fixture.transport->requests.front().body).at("license") == file);
}

TEST_CASE("migrate_license returns a trial the endpoint migrated as one")
{
    migration_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["trial"] = true;
    claims["exp"] = moonbase::tests::now_seconds() + 5 * 86400;
    fixture.respond(http_response{200, {}, fixture.token(claims)});

    const auto trial = fixture.client().migrate_license("trial-key");

    CHECK(trial.trial);
    REQUIRE(trial.expires_at.has_value());
    CHECK(*trial.expires_at > std::chrono::system_clock::now());
}

TEST_CASE("migrate_license refuses a license it can't send, unsent")
{
    migration_fixture fixture;

    SUBCASE("a blank one")
    {
        CHECK_THROWS_AS((void)fixture.client().migrate_license(" \r\n\t"), configuration_error);
    }

    SUBCASE("one that is not UTF-8 text")
    {
        CHECK_THROWS_AS((void)fixture.client().migrate_license(std::string("key-\xff\xfe", 6)), configuration_error);
    }

    SUBCASE("for a device with no id")
    {
        fixture.device_ids = std::make_shared<static_device_id_resolver>("Test Device", " ");
        CHECK_THROWS_AS((void)fixture.client().migrate_license("ABCD-1234"), configuration_error);
    }

    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("migrate_license says so when the endpoint named another device")
{
    migration_fixture fixture;
    fixture.respond(http_response{200, {}, fixture.migrated("device-the-old-system-knew")});

    try {
        (void)fixture.client().migrate_license("ABCD-1234");
        FAIL("expected the migration to throw");
    } catch (const license_device_mismatch_error& ex) {
        CHECK(ex.type() == error_type::license_device_mismatch);
        const std::string message = ex.what();
        CHECK(message.find("This license is not for this device") == 0);
        CHECK(message.find("deviceSignature it was sent") != std::string::npos);
    }
}

TEST_CASE("migrate_license reports a license that has ended as expired")
{
    migration_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["exp"] = moonbase::tests::now_seconds() - 60;
    fixture.respond(http_response{200, {}, fixture.token(claims)});

    CHECK_THROWS_AS((void)fixture.client().migrate_license("ABCD-1234"), license_expired_error);
}

TEST_CASE("migrate_license maps Moonbase's refusals")
{
    migration_fixture fixture;

    SUBCASE("the endpoint refused the license")
    {
        fixture.refuse(400, "LicenseInvalid", "License is not valid");
        CHECK_THROWS_WITH_AS((void)fixture.client().migrate_license("ABCD-1234"), "License is not valid",
                             license_invalid_error);
    }

    SUBCASE("the customer has no license with a seat free")
    {
        fixture.refuse(400, "NoEligibleLicense", "No license with available activations found");
        CHECK_THROWS_WITH_AS((void)fixture.client().migrate_license("ABCD-1234"),
                             "No license with available activations found", no_eligible_license_error);
    }

    SUBCASE("the license has ended")
    {
        fixture.refuse(400, "LicenseExpired", "License has expired");
        CHECK_THROWS_AS((void)fixture.client().migrate_license("ABCD-1234"), license_expired_error);
    }

    SUBCASE("no license of the customer's covers this version")
    {
        fixture.respond(http_response{
            400,
            {},
            nlohmann::json{
                {"title", "Release not covered"},
                {"status", 400},
                {"detail", "Your license doesn't cover version 1.2.3"},
                {"errorType", "ReleaseNotCovered"},
                {"appVersion", "1.2.3"},
                {"releaseAccess", {{"upToMajor", 0}}},
                {"upgradeUrl", "https://demo.moonbase.sh/upgrade"},
            }
                .dump()});
        try {
            (void)fixture.client().migrate_license("ABCD-1234");
            FAIL("expected the migration to throw");
        } catch (const release_not_covered_error& ex) {
            CHECK_FALSE(ex.license().has_value());
            CHECK(ex.app_version() == "1.2.3");
            REQUIRE(ex.upgrade_url().has_value());
            CHECK(*ex.upgrade_url() == "https://demo.moonbase.sh/upgrade");
        }
    }

    SUBCASE("the store has closed")
    {
        fixture.refuse(410, "StoreClosed", "This store has closed.");
        CHECK_THROWS_AS((void)fixture.client().migrate_license("ABCD-1234"), store_closed_error);
    }
}

TEST_CASE("migrate_license keeps every other failure an api_error")
{
    migration_fixture fixture;
    int expected_status = 0;

    SUBCASE("no such customer or product: an empty 404")
    {
        expected_status = 404;
        fixture.respond(http_response{404, {}, ""});
    }

    SUBCASE("license migration isn't set up")
    {
        expected_status = 500;
        fixture.refuse(500, "", "No suitable license migration found");
    }

    SUBCASE("the device already has a license of the product")
    {
        expected_status = 409;
        fixture.refuse(409, "", "Machine already has a license for this product");
    }

    SUBCASE("a captive portal answered")
    {
        expected_status = 200;
        fixture.respond(http_response{200, {}, "<html><body>Sign in to the Wi-Fi</body></html>"});
    }

    try {
        (void)fixture.client().migrate_license("ABCD-1234");
        FAIL("expected the migration to throw");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == expected_status);
    }
}

TEST_CASE("licensing migrates a license and leaves storing it to the caller")
{
    migration_fixture fixture;
    auto store = std::make_shared<memory_license_store>();
    licensing instance(fixture.options(), store, fixture.device_ids, fixture.transport);
    const auto token = fixture.migrated();
    fixture.respond(http_response{200, {}, token});

    const auto migrated = instance.migrate_license("ABCD-1234");

    CHECK(migrated.token == token);
    CHECK_FALSE(store->load_local_license().has_value());

    // Once stored, it validates locally like any license.
    store->store_local_license(migrated);
    CHECK(instance.validate_token_local(token).activation_id == "activation-123");
}
