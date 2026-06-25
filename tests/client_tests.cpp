#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <memory>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/fingerprint.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

struct client_fixture {
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    licensing_options options;
    std::shared_ptr<static_fingerprint_provider> fingerprints;
    std::shared_ptr<license_validator> validator;
    std::shared_ptr<moonbase::tests::recording_transport> transport;
    license_client client;

    explicit client_fixture(std::deque<http_response> responses)
        : options(),
          fingerprints(std::make_shared<static_fingerprint_provider>("Test Device", "device-id")),
          validator(),
          transport(std::make_shared<moonbase::tests::recording_transport>(std::move(responses))),
          client(make_options(), fingerprints, make_validator(), transport)
    {
    }

    licensing_options make_options()
    {
        options.endpoint = "https://demo.moonbase.sh/";
        options.product_id = "demo-app";
        options.public_key = key.public_pem;
        options.account_id = "tenant-1";
        options.target_platform = platform::mac;
        options.application_version = "1.2.3";
        options.client_info = "moonbase-juce/9.9 (JUCE v8; TestOS)";
        options.metadata = {{"channel", "test"}};
        options.http_connect_timeout = std::chrono::milliseconds{1234};
        options.http_request_timeout = std::chrono::milliseconds{5678};
        return options;
    }

    std::shared_ptr<license_validator> make_validator()
    {
        validator = std::make_shared<license_validator>(make_options(), fingerprints);
        return validator;
    }
};

} // namespace

TEST_CASE("request_activation posts device information and parses response")
{
    client_fixture fixture({
        http_response{
            200,
            {},
            R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"},
    });

    const auto response = fixture.client.request_activation();

    CHECK(response.id == "request-123");
    CHECK(response.request_url.find("request-123") != std::string::npos);
    CHECK(response.browser_url.find("activate") != std::string::npos);

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.method == "POST");
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/activations/demo-app/request?") == 0);
    CHECK(request.url.find("format=JWT") != std::string::npos);
    CHECK(request.url.find("platform=Mac") != std::string::npos);
    CHECK(request.url.find("appVersion=1.2.3") != std::string::npos);
    CHECK(request.url.find("meta%5Bchannel%5D=test") != std::string::npos);
    CHECK(request.headers.at("Content-Type") == "application/json");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");
    CHECK(request.headers.at("User-Agent").find("moonbase-cpp/") == 0); // base prefix preserved
    CHECK(request.headers.at("User-Agent").find("moonbase-juce/9.9 (JUCE v8; TestOS)") != std::string::npos);
    CHECK(request.connect_timeout == std::chrono::milliseconds{1234});
    CHECK(request.request_timeout == std::chrono::milliseconds{5678});

    const auto body = nlohmann::json::parse(request.body);
    CHECK(body.at("deviceName") == "Test Device");
    CHECK(body.at("deviceSignature") == "device-id");
}

TEST_CASE("request_activation throws for API errors")
{
    client_fixture fixture({
        http_response{500, {}, R"({"title":"Failure","detail":"Backend failed"})"},
    });

    CHECK_THROWS_AS((void)fixture.client.request_activation(), api_error);
}

TEST_CASE("get_requested_activation returns nullopt while pending or missing")
{
    client_fixture fixture({
        http_response{204, {}, ""},
        http_response{404, {}, ""},
    });

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};

    CHECK_FALSE(fixture.client.get_requested_activation(request).has_value());
    CHECK_FALSE(fixture.client.get_requested_activation(request).has_value());
    REQUIRE(fixture.transport->requests.size() == 2);
    CHECK(fixture.transport->requests[0].method == "GET");
    CHECK(fixture.transport->requests[0].connect_timeout == std::chrono::milliseconds{1234});
    CHECK(fixture.transport->requests[0].request_timeout == std::chrono::milliseconds{5678});
}

TEST_CASE("get_requested_activation validates fulfilled JWT response")
{
    client_fixture fixture({});
    const auto token = moonbase::tests::make_token(
        fixture.key.key.get(),
        moonbase::tests::default_claims());
    fixture.transport->responses.push_back(http_response{200, {}, token});

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    const auto result = fixture.client.get_requested_activation(request);

    REQUIRE(result.has_value());
    CHECK(result->id == "license-123");
}

TEST_CASE("get_requested_activation maps license problem details")
{
    client_fixture fixture({
        http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"The license has expired"})"},
    });

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    CHECK_THROWS_AS((void)fixture.client.get_requested_activation(request), license_expired_error);
}

TEST_CASE("validate_token_online posts the JWT and parses the refreshed response")
{
    client_fixture fixture({});
    const auto refreshed = moonbase::tests::make_token(
        fixture.key.key.get(),
        moonbase::tests::default_claims());
    fixture.transport->responses.push_back(http_response{200, {}, refreshed});

    const auto result = fixture.client.validate_token_online("original.jwt.token");

    CHECK(result.id == "license-123");
    CHECK(result.token == refreshed);

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.method == "POST");
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/licenses/demo-app/validate?") == 0);
    CHECK(request.url.find("format=JWT") != std::string::npos);
    CHECK(request.url.find("platform=Mac") != std::string::npos);
    CHECK(request.url.find("appVersion=1.2.3") != std::string::npos);
    CHECK(request.url.find("meta%5Bchannel%5D=test") != std::string::npos);
    CHECK(request.headers.at("Content-Type") == "text/plain");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");
    CHECK(request.connect_timeout == std::chrono::milliseconds{1234});
    CHECK(request.request_timeout == std::chrono::milliseconds{5678});
    CHECK(request.body == "original.jwt.token");
}

TEST_CASE("validate_token_online maps license problem details")
{
    SUBCASE("expired")
    {
        client_fixture fixture({
            http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"The license has expired"})"},
        });
        CHECK_THROWS_AS((void)fixture.client.validate_token_online("token"), license_expired_error);
    }

    SUBCASE("invalid")
    {
        client_fixture fixture({
            http_response{400, {}, R"({"title":"Invalid","detail":"Token is not valid"})"},
        });
        CHECK_THROWS_AS((void)fixture.client.validate_token_online("token"), license_invalid_error);
    }
}

TEST_CASE("validate_token_online re-validates the refreshed JWT locally")
{
    client_fixture fixture({});
    auto claims = moonbase::tests::default_claims("other-device");
    const auto refreshed = moonbase::tests::make_token(fixture.key.key.get(), claims);
    fixture.transport->responses.push_back(http_response{200, {}, refreshed});

    CHECK_THROWS_AS((void)fixture.client.validate_token_online("token"), license_invalid_error);
}

TEST_CASE("revoke_activation posts the JWT to the revoke endpoint")
{
    client_fixture fixture({
        http_response{200, {}, ""},
    });

    fixture.client.revoke_activation("original.jwt.token");

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.method == "POST");
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/licenses/demo-app/revoke?") == 0);
    CHECK(request.url.find("format=JWT") != std::string::npos);
    CHECK(request.url.find("platform=Mac") != std::string::npos);
    CHECK(request.url.find("appVersion=1.2.3") != std::string::npos);
    CHECK(request.url.find("meta%5Bchannel%5D=test") != std::string::npos);
    CHECK(request.headers.at("Content-Type") == "text/plain");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");
    CHECK(request.connect_timeout == std::chrono::milliseconds{1234});
    CHECK(request.request_timeout == std::chrono::milliseconds{5678});
    CHECK(request.body == "original.jwt.token");
}

TEST_CASE("revoke_activation maps API errors")
{
    SUBCASE("invalid")
    {
        client_fixture fixture({
            http_response{400, {}, R"({"title":"Invalid","detail":"Token is not valid"})"},
        });
        CHECK_THROWS_AS(fixture.client.revoke_activation("token"), license_invalid_error);
    }

    SUBCASE("server error")
    {
        client_fixture fixture({
            http_response{500, {}, R"({"title":"Failure","detail":"Backend failed"})"},
        });
        CHECK_THROWS_AS(fixture.client.revoke_activation("token"), api_error);
    }
}
