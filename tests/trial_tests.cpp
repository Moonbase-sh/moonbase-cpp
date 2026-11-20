#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/store.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

struct trial_fixture {
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

    // A trial as Moonbase issues one to an anonymous device, ending in `ends_in`
    // seconds (negative for one that has ended).
    [[nodiscard]] std::string trial_token(long long ends_in = 14 * 86400,
                                          const std::string& device_id = "device-id") const
    {
        auto claims = moonbase::tests::default_claims(device_id);
        const auto now = moonbase::tests::now_seconds();
        claims["trial"] = true;
        claims["id"] = "trial-123";
        claims["l:id"] = "trial-123";
        claims["iat"] = now - 20 * 86400;
        claims["nbf"] = now - 20 * 86400;
        claims["exp"] = now + ends_in;
        claims["u:id"] = "00000000-0000-0000-0000-000000000000";
        claims["u:name"] = "";
        claims["u:email"] = "";
        claims["t:properties"] = nlohmann::json::object();
        claims.erase("s:id");
        claims.erase("sp:owned");
        return moonbase::tests::make_token(key.key.get(), claims);
    }

    [[nodiscard]] license_client client() const
    {
        const auto with = options();
        return license_client(with, device_ids, std::make_shared<license_validator>(with, device_ids), transport);
    }

    void respond(http_response response) { transport->responses.push_back(std::move(response)); }
};

// What Moonbase answers a trial it won't start on the device directly with:
// Problem(DomainViolationException), with no errorType.
http_response refusal(int status, const std::string& title, const std::string& detail)
{
    return http_response{
        status,
        {},
        nlohmann::json{{"title", title}, {"status", status}, {"detail", detail}, {"instance", "demo-app"}}.dump()};
}

std::string query_of(const std::string& url)
{
    const auto at = url.find('?');
    return at == std::string::npos ? std::string() : url.substr(at + 1);
}

} // namespace

// Code that catches license_expired_error to lock on an ended trial sees it too.
static_assert(std::is_base_of_v<license_expired_error, trial_expired_error>);

TEST_CASE("request_trial posts the device to the trial endpoint and returns the trial")
{
    trial_fixture fixture;
    const auto token = fixture.trial_token();
    fixture.respond(http_response{200, {{"Content-Type", "application/jwt"}}, token});

    const auto trial = fixture.client().request_trial();

    CHECK(trial.trial);
    CHECK(trial.activation_id == "trial-123");
    CHECK(trial.licensed_product.id == "demo-app");
    REQUIRE(trial.expires_at.has_value());
    CHECK(*trial.expires_at > std::chrono::system_clock::now());
    CHECK(trial.token == token);

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.method == "POST");
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/trials/demo-app/request?") == 0);
    const auto query = query_of(request.url);
    CHECK(query.find("format=JWT") != std::string::npos);
    CHECK(query.find("platform=Mac") != std::string::npos);
    CHECK(query.find("appVersion=1.2.3") != std::string::npos);
    CHECK(query.find("meta%5Bchannel%5D=test") != std::string::npos);
    CHECK(query.find("utm_") == std::string::npos);
    CHECK(query.find("method=") == std::string::npos); // the activation request's alone
    CHECK(request.headers.at("Content-Type") == "application/json");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");
    CHECK(request.headers.at("User-Agent").find("moonbase-cpp/") == 0);

    const auto body = nlohmann::json::parse(request.body);
    CHECK(body.at("deviceName") == "Test Device");
    CHECK(body.at("deviceSignature") == "device-id");
}

TEST_CASE("request_trial sends the UTM tags it is given, and only those")
{
    trial_fixture fixture;
    fixture.respond(http_response{200, {}, fixture.trial_token()});

    request_trial_options options;
    options.utm.source = "newsletter";
    options.utm.medium = "email";
    options.utm.campaign = "spring launch";
    options.utm.referrer = "https://example.com/blog?post=1";
    (void)fixture.client().request_trial(options);

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto query = query_of(fixture.transport->requests.front().url);
    CHECK(query.find("utm_source=newsletter") != std::string::npos);
    CHECK(query.find("utm_medium=email") != std::string::npos);
    CHECK(query.find("utm_campaign=spring%20launch") != std::string::npos);
    CHECK(query.find("utm_referrer=https%3A%2F%2Fexample.com%2Fblog%3Fpost%3D1") != std::string::npos);
    CHECK(query.find("utm_term") == std::string::npos);
    CHECK(query.find("utm_content") == std::string::npos);
    CHECK(query.find("format=JWT") != std::string::npos);
}

TEST_CASE("request_trial returns a trial that never ends")
{
    trial_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["trial"] = true;
    claims.erase("exp");
    fixture.respond(http_response{200, {}, moonbase::tests::make_token(fixture.key.key.get(), claims)});

    const auto trial = fixture.client().request_trial();

    CHECK(trial.trial);
    CHECK_FALSE(trial.expires_at.has_value());
}

TEST_CASE("request_trial reports the device's ended trial with the trial itself")
{
    trial_fixture fixture;
    const auto ended = fixture.trial_token(-3 * 86400);
    fixture.respond(http_response{200, {}, ended});

    try {
        (void)fixture.client().request_trial();
        FAIL("expected an ended trial to throw");
    } catch (const trial_expired_error& ex) {
        CHECK(ex.type() == error_type::license_expired);
        CHECK(std::string(ex.what()) == "The trial on this device has ended");
        CHECK(ex.trial().trial);
        CHECK(ex.trial().activation_id == "trial-123");
        CHECK(ex.trial().token == ended);
        REQUIRE(ex.trial().expires_at.has_value());
        CHECK(*ex.trial().expires_at < std::chrono::system_clock::now());
    }
}

TEST_CASE("request_trial still verifies an ended trial")
{
    // Signed for another device: nothing to show, ended or not.
    trial_fixture fixture;
    fixture.respond(http_response{200, {}, fixture.trial_token(-86400, "another-device")});

    CHECK_THROWS_AS((void)fixture.client().request_trial(), license_device_mismatch_error);
}

TEST_CASE("request_trial refuses a trial signed by another key")
{
    trial_fixture fixture;
    const auto other = moonbase::tests::generate_key();
    auto claims = moonbase::tests::default_claims();
    claims["trial"] = true;
    fixture.respond(http_response{200, {}, moonbase::tests::make_token(other.key.get(), claims)});

    CHECK_THROWS_AS((void)fixture.client().request_trial(), license_invalid_error);
}

TEST_CASE("request_trial reports a trial Moonbase won't start here as not supported")
{
    trial_fixture fixture;

    SUBCASE("the product offers no trials")
    {
        fixture.respond(refusal(400, "Not allowed", "Product does not allow trials"));
        CHECK_THROWS_WITH_AS((void)fixture.client().request_trial(), "Product does not allow trials",
                             operation_not_supported_error);
    }

    SUBCASE("its trials need a signed-in customer")
    {
        fixture.respond(refusal(401, "Not authenticated",
                                "You need to be logged in to request trials for this product"));
        CHECK_THROWS_WITH_AS((void)fixture.client().request_trial(),
                             "You need to be logged in to request trials for this product",
                             operation_not_supported_error);
    }

    SUBCASE("its trials need a newsletter opt-in")
    {
        fixture.respond(refusal(403, "Newsletter opt-in required",
                                "You need to opt in to the newsletter to request trials for this product"));
        CHECK_THROWS_WITH_AS((void)fixture.client().request_trial(),
                             "You need to opt in to the newsletter to request trials for this product",
                             operation_not_supported_error);
    }
}

TEST_CASE("request_trial keeps a failure that isn't Moonbase's refusal retryable")
{
    trial_fixture fixture;
    int expected_status = 0;

    SUBCASE("a proxy's block page")
    {
        expected_status = 403;
        fixture.respond(http_response{403, {}, "<html><body>Access denied</body></html>"});
    }

    SUBCASE("a 401 with no body")
    {
        expected_status = 401;
        fixture.respond(http_response{401, {}, ""});
    }

    // What the gateway and firewall in front of the API answer with: JSON, but
    // not Moonbase's problem, so no word on the product's trials.
    SUBCASE("the firewall blocking the request")
    {
        expected_status = 403;
        fixture.respond(http_response{403, {}, R"({"message":"Forbidden"})"});
    }

    SUBCASE("the gateway refusing a misrouted request")
    {
        expected_status = 403;
        fixture.respond(http_response{403, {}, R"({"message":"Missing Authentication Token"})"});
    }

    SUBCASE("the gateway's 401")
    {
        expected_status = 401;
        fixture.respond(http_response{401, {}, R"({"message":"Unauthorized"})"});
    }

    SUBCASE("a problem whose status isn't the response's")
    {
        expected_status = 403;
        fixture.respond(http_response{403, {}, R"({"title":"Bad Request","status":400,"detail":"Not this one"})"});
    }

    SUBCASE("no such product")
    {
        expected_status = 404;
        fixture.respond(http_response{404, {}, ""});
    }

    SUBCASE("two requests racing for the device's first trial")
    {
        expected_status = 409;
        fixture.respond(refusal(409, "Conflict", "Machine already has a trial for this product"));
    }

    SUBCASE("rate limited")
    {
        expected_status = 429;
        fixture.respond(http_response{429, {{"Retry-After", "30"}}, ""});
    }

    SUBCASE("a server error")
    {
        expected_status = 500;
        fixture.respond(http_response{500, {}, R"({"title":"Internal Server Error"})"});
    }

    try {
        (void)fixture.client().request_trial();
        FAIL("expected the request to throw");
    } catch (const operation_not_supported_error&) {
        FAIL("only Moonbase's own refusal says the trial can't start here");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == expected_status);
    }
}

TEST_CASE("request_trial reports a closed store")
{
    trial_fixture fixture;
    fixture.respond(http_response{
        410,
        {},
        R"({"title":"Store closed","status":410,"detail":"This store has closed.","errorType":"StoreClosed"})"});

    CHECK_THROWS_WITH_AS((void)fixture.client().request_trial(), "This store has closed.", store_closed_error);
}

TEST_CASE("request_trial treats a 200 that is not a token as retryable")
{
    trial_fixture fixture;
    fixture.respond(http_response{200, {}, "<html><body>Sign in to the Wi-Fi</body></html>"});

    try {
        (void)fixture.client().request_trial();
        FAIL("expected the request to throw");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == 200);
    }
}

TEST_CASE("request_trial sends the device as request_activation does")
{
    trial_fixture fixture;

    SUBCASE("a blank device name is stood in for")
    {
        fixture.device_ids = std::make_shared<static_device_id_resolver>("  ", "device-id");
        fixture.respond(http_response{200, {}, fixture.trial_token()});
        (void)fixture.client().request_trial();
        REQUIRE(fixture.transport->requests.size() == 1);
        CHECK(nlohmann::json::parse(fixture.transport->requests.front().body).at("deviceName") == "Unknown device");
    }

    SUBCASE("an empty device id is refused unsent")
    {
        fixture.device_ids = std::make_shared<static_device_id_resolver>("Test Device", " ");
        CHECK_THROWS_AS((void)fixture.client().request_trial(), configuration_error);
        CHECK(fixture.transport->requests.empty());
    }

    SUBCASE("a device id that is not UTF-8 is refused unsent")
    {
        fixture.device_ids = std::make_shared<static_device_id_resolver>("Test Device", "device-\xff");
        CHECK_THROWS_AS((void)fixture.client().request_trial(), configuration_error);
        CHECK(fixture.transport->requests.empty());
    }
}

TEST_CASE("licensing starts a trial and leaves storing it to the caller")
{
    trial_fixture fixture;
    auto store = std::make_shared<memory_license_store>();
    licensing instance(fixture.options(), store, fixture.device_ids, fixture.transport);
    const auto token = fixture.trial_token();
    fixture.respond(http_response{200, {}, token});

    request_trial_options options;
    options.utm.source = "installer";
    const auto trial = instance.request_trial(options);

    CHECK(trial.trial);
    CHECK(trial.token == token);
    CHECK_FALSE(store->load_local_license().has_value());
    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(query_of(fixture.transport->requests.front().url).find("utm_source=installer") != std::string::npos);

    // Once stored, it validates locally like any license.
    store->store_local_license(trial);
    CHECK(instance.validate_token_local(token).trial);
}
