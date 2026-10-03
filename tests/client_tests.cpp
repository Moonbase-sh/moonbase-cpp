#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/inventory.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

struct client_fixture {
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    licensing_options options;
    std::shared_ptr<static_device_id_resolver> fingerprints;
    std::shared_ptr<license_validator> validator;
    std::shared_ptr<moonbase::tests::recording_transport> transport;
    license_client client;

    explicit client_fixture(std::deque<http_response> responses)
        : options(),
          fingerprints(std::make_shared<static_device_id_resolver>("Test Device", "device-id")),
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
    CHECK(request.url.find("method=Online") != std::string::npos);
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

    CHECK(response.method == activation_method::online);
}

TEST_CASE("client_info is appended to the User-Agent, sanitised and capped")
{
    licensing_options options;

    SUBCASE("layers read outermost-last after the base segment")
    {
        options.client_info = "moonbase-juce/9.9 (JUCE v8; TestOS) HISE/4.1.0";
        const auto ua = detail::default_headers(options).at("User-Agent");
        CHECK(ua.find("moonbase-cpp/") == 0);
        CHECK(ua.find("moonbase-juce/9.9") < ua.find("HISE/4.1.0"));
    }

    SUBCASE("CR/LF cannot inject a second header")
    {
        options.client_info = "HISE/4.1.0\r\nX-Injected: 1";
        const auto ua = detail::default_headers(options).at("User-Agent");
        CHECK(ua.find('\r') == std::string::npos);
        CHECK(ua.find('\n') == std::string::npos);
        CHECK(ua.find("HISE/4.1.0 X-Injected: 1") != std::string::npos);
    }

    SUBCASE("an embedded NUL cannot truncate the header")
    {
        options.client_info = std::string("HISE/4.1.0\0hidden", 17);
        CHECK(detail::default_headers(options).at("User-Agent").find("HISE/4.1.0 hidden") !=
              std::string::npos);
    }

    SUBCASE("a segment that sanitises away leaves no trailing space")
    {
        options.client_info = "  \r\n\t ";
        CHECK(detail::default_headers(options).at("User-Agent") ==
              "moonbase-cpp/" + detail::version_string());
    }

    SUBCASE("whitespace runs collapse to a single separator")
    {
        options.client_info = "  HISE/4.1.0   MyWrapper/2.0  ";
        CHECK(detail::default_headers(options).at("User-Agent").find("HISE/4.1.0 MyWrapper/2.0") !=
              std::string::npos);
    }

    SUBCASE("an oversized segment is capped rather than dropped")
    {
        options.client_info = std::string(1000, 'x');
        const auto ua = detail::default_headers(options).at("User-Agent");
        CHECK(ua.find("moonbase-cpp/") == 0);
        CHECK(ua.size() < 320); // base segment + the 256-char cap
        CHECK(ua.find("xxx") != std::string::npos);
    }
}

TEST_CASE("request_activation asks for an offline license")
{
    client_fixture fixture({
        http_response{
            200,
            {},
            R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"},
    });

    const auto response = fixture.client.request_activation(activation_method::offline);

    // The response carries no method, so the client records what it asked for.
    CHECK(response.method == activation_method::offline);
    CHECK(response.id == "request-123");

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.url.find("method=Offline") != std::string::npos);
    CHECK(request.url.find("method=Online") == std::string::npos);

    // Everything other than the method is unchanged from the online request.
    CHECK(request.method == "POST");
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/activations/demo-app/request?") == 0);
    CHECK(request.url.find("format=JWT") != std::string::npos);
    CHECK(request.headers.at("Content-Type") == "application/json");

    const auto body = nlohmann::json::parse(request.body);
    CHECK(body.at("deviceName") == "Test Device");
    CHECK(body.at("deviceSignature") == "device-id");
}

TEST_CASE("request_activation reports a product that disallows offline activations")
{
    client_fixture fixture({
        http_response{
            400,
            {},
            R"({"title":"Not allowed","detail":"Product does not allow offline activations"})"},
    });

    CHECK_THROWS_WITH_AS(
        (void)fixture.client.request_activation(activation_method::offline),
        "Product does not allow offline activations",
        license_invalid_error);
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
    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};

    // A fresh client per status: a second poll on the same client would be held
    // back by the activation poll floor before it reached the transport.
    for (const long status : {204L, 404L}) {
        CAPTURE(status);
        client_fixture fixture({http_response{status, {}, ""}});

        CHECK_FALSE(fixture.client.get_requested_activation(request).has_value());
        REQUIRE(fixture.transport->requests.size() == 1);
        CHECK(fixture.transport->requests[0].method == "GET");
        CHECK(fixture.transport->requests[0].connect_timeout == std::chrono::milliseconds{1234});
        CHECK(fixture.transport->requests[0].request_timeout == std::chrono::milliseconds{5678});
    }
}

TEST_CASE("copies of a client share the activation poll floor")
{
    client_fixture fixture({http_response{204, {}, ""}});
    const auto copy = fixture.client;

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};

    CHECK_FALSE(fixture.client.get_requested_activation(request).has_value());
    CHECK_FALSE(copy.get_requested_activation(request).has_value());
    CHECK(fixture.transport->requests.size() == 1);
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

TEST_CASE("a 400 from the activation poll ends the request with the server's reason")
{
    client_fixture fixture({
        http_response{400, {}, R"({"title":"Invalid state","detail":"Activation request was cancelled"})"},
    });

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    CHECK_THROWS_WITH_AS(
        (void)fixture.client.get_requested_activation(request),
        "Activation request can no longer be completed: Activation request was cancelled",
        activation_request_error);
}

TEST_CASE("an expired activation request is not reported as an expired license")
{
    // What the API answers once a request has waited an hour unfulfilled.
    client_fixture fixture({
        http_response{
            400,
            {},
            R"({"title":"Invalid state","status":400,"detail":"This activation request has expired. Start the activation again from the app."})"},
    });

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    CHECK_THROWS_WITH_AS(
        (void)fixture.client.get_requested_activation(request),
        "Activation request can no longer be completed: This activation request has expired. "
        "Start the activation again from the app.",
        activation_request_error);
}

TEST_CASE("a 400 without a problem body still ends the activation request")
{
    client_fixture fixture({http_response{400, {}, ""}});

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    CHECK_THROWS_WITH_AS(
        (void)fixture.client.get_requested_activation(request),
        "Activation request can no longer be completed (HTTP 400)",
        activation_request_error);
}

TEST_CASE("an ended activation request has its own error type, not license_invalid")
{
    client_fixture fixture({
        http_response{400, {}, R"({"detail":"Activation request was cancelled"})"},
    });

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    try {
        (void)fixture.client.get_requested_activation(request);
        FAIL("expected the poll to throw");
    } catch (const moonbase_error& ex) {
        CHECK(dynamic_cast<const activation_request_error*>(&ex) != nullptr);
        CHECK(dynamic_cast<const license_invalid_error*>(&ex) == nullptr);
        CHECK(ex.type() == error_type::activation_request_ended);
    }
}

TEST_CASE("server errors from the activation poll stay retryable")
{
    client_fixture fixture({
        http_response{503, {}, R"({"title":"Service Unavailable"})"},
    });

    activation_request request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
    try {
        (void)fixture.client.get_requested_activation(request);
        FAIL("expected the poll to throw");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == 503);
    }
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
    // "method" is only accepted on the request endpoint.
    CHECK(request.url.find("method=") == std::string::npos);
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
    // "method" is only accepted on the request endpoint.
    CHECK(request.url.find("method=") == std::string::npos);
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

// ---------------------------------------------------------------------------
// Every failure shape the API and what sits in front of it can produce.
// ---------------------------------------------------------------------------

namespace {

// ASP.NET's NotFound() as the API serves it.
constexpr const char* api_not_found =
    R"({"type":"https://tools.ietf.org/html/rfc9110#section-15.5.5","title":"Not Found","status":404,"traceId":"00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"})";

activation_request poll_request()
{
    return activation_request{"request-123", "https://demo.moonbase.sh/api/client/activations/request-123?format=JWT", ""};
}

} // namespace

TEST_CASE("a numeric errorType is not read as an expired license")
{
    // DomainErrorCode 4 is LicenseInvalid; the API sends names, never numbers.
    client_fixture fixture({
        http_response{400, {}, R"({"title":"Invalid","detail":"License is not valid","errorType":4})"},
    });
    CHECK_THROWS_WITH_AS(
        (void)fixture.client.validate_token_online("token"),
        "License is not valid",
        license_invalid_error);
}

TEST_CASE("validate_token_online reports a revoked license or activation as invalid")
{
    SUBCASE("license revoked")
    {
        client_fixture fixture({
            http_response{400, {}, R"({"title":"Invalid state","detail":"License has been revoked","status":400,"errorType":"LicenseRevoked"})"},
        });
        CHECK_THROWS_WITH_AS(
            (void)fixture.client.validate_token_online("token"),
            "License has been revoked",
            license_invalid_error);
    }

    SUBCASE("activation revoked")
    {
        client_fixture fixture({
            http_response{400, {}, R"({"title":"Not allowed","detail":"License has been revoked","status":400,"errorType":"LicenseActivationRevoked"})"},
        });
        CHECK_THROWS_WITH_AS(
            (void)fixture.client.validate_token_online("token"),
            "License has been revoked",
            license_invalid_error);
    }

    SUBCASE("license no longer active (a pending license, or an older server)")
    {
        client_fixture fixture({
            http_response{400, {}, R"({"title":"Invalid state","detail":"License is no longer active","status":400})"},
        });
        CHECK_THROWS_WITH_AS(
            (void)fixture.client.validate_token_online("token"),
            "License is no longer active",
            license_invalid_error);
    }
}

TEST_CASE("a 403 or 404 on validate or revoke stays retryable, even as ProblemDetails")
{
    // Moonbase sends these for a license that no longer exists or changed hands,
    // but as stock ASP.NET ProblemDetails with no errorType: any other ASP.NET
    // service a request is misrouted to answers the same way. Reading them as
    // final would skip the offline grace period.
    SUBCASE("validate, 404")
    {
        client_fixture fixture({http_response{404, {}, api_not_found}});
        try {
            (void)fixture.client.validate_token_online("token");
            FAIL("expected validation to throw");
        } catch (const license_invalid_error&) {
            FAIL("an anonymous 404 must not read as a verdict");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 404);
        }
    }

    SUBCASE("revoke, 404")
    {
        client_fixture fixture({http_response{404, {}, api_not_found}});
        CHECK_THROWS_AS(fixture.client.revoke_activation("token"), api_error);
    }

    SUBCASE("revoke, 403 from a license that changed hands")
    {
        client_fixture fixture({
            http_response{403, {}, R"({"title":"Not allowed","detail":"User does not own license","status":403})"},
        });
        try {
            fixture.client.revoke_activation("token");
            FAIL("expected the revoke to throw");
        } catch (const license_invalid_error&) {
            FAIL("an anonymous 403 must not read as a verdict");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 403);
            CHECK(std::string(ex.what()) == "User does not own license");
        }
    }
}

TEST_CASE("a 404 the API did not write stays retryable")
{
    for (const std::string body : {std::string{}, std::string{"<html><body>Not Found</body></html>"}}) {
        CAPTURE(body);
        client_fixture fixture({http_response{404, {}, body}});
        try {
            (void)fixture.client.validate_token_online("token");
            FAIL("expected validation to throw");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 404);
            CHECK(std::string(ex.what()) == "Moonbase API request failed with HTTP 404");
        }
    }
}

TEST_CASE("a proxy's 403 block page stays retryable")
{
    client_fixture fixture({
        http_response{403, {}, "<!DOCTYPE html><html><body>Blocked by policy</body></html>"},
    });
    try {
        (void)fixture.client.validate_token_online("token");
        FAIL("expected validation to throw");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == 403);
    }
}

TEST_CASE("a 200 that is not a token is retryable, not an invalid license")
{
    const std::string portal = "<html><head><title>Hotel Wi-Fi</title></head><body>Sign in</body></html>";

    SUBCASE("validate")
    {
        client_fixture fixture({http_response{200, {}, portal}});
        try {
            (void)fixture.client.validate_token_online("token");
            FAIL("expected validation to throw");
        } catch (const license_invalid_error&) {
            FAIL("a captive portal must not read as an invalid license");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 200);
        }
    }

    SUBCASE("activation poll")
    {
        client_fixture fixture({http_response{200, {}, portal}});
        try {
            (void)fixture.client.get_requested_activation(poll_request());
            FAIL("expected the poll to throw");
        } catch (const license_invalid_error&) {
            FAIL("a captive portal must not end the activation");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 200);
        }
    }

    SUBCASE("revoke")
    {
        client_fixture fixture({http_response{200, {}, portal}});
        CHECK_THROWS_AS(fixture.client.revoke_activation("token"), api_error);
    }
}

TEST_CASE("a token that is well formed but wrongly signed is still an invalid license")
{
    client_fixture fixture({});
    const auto other = moonbase::tests::generate_key();
    fixture.transport->responses.push_back(
        http_response{200, {}, moonbase::tests::make_token(other.key.get(), moonbase::tests::default_claims())});

    CHECK_THROWS_AS((void)fixture.client.validate_token_online("token"), license_invalid_error);
}

TEST_CASE("a bare-string 400 surfaces its message")
{
    SUBCASE("as JSON")
    {
        client_fixture fixture({http_response{400, {}, R"("Trials can not be revoked")"}});
        CHECK_THROWS_WITH_AS(
            fixture.client.revoke_activation("token"),
            "Trials can not be revoked",
            license_invalid_error);
    }

    SUBCASE("as plain text")
    {
        client_fixture fixture({http_response{400, {}, "Trials can not be revoked"}});
        CHECK_THROWS_WITH_AS(
            fixture.client.revoke_activation("token"),
            "Trials can not be revoked",
            license_invalid_error);
    }
}

TEST_CASE("a validation 400 quotes its first field error")
{
    client_fixture fixture({
        http_response{
            400,
            {},
            R"({"type":"https://tools.ietf.org/html/rfc9110#section-15.5.1","title":"One or more validation errors occurred.","status":400,"errors":{"deviceName":["The DeviceName field is required."]}})"},
    });
    CHECK_THROWS_WITH_AS(
        (void)fixture.client.request_activation(),
        "deviceName: The DeviceName field is required.",
        license_invalid_error);
}

TEST_CASE("gateway and server failures stay retryable and say what they can")
{
    SUBCASE("gateway timeout")
    {
        client_fixture fixture({http_response{504, {}, R"({"message":"Endpoint request timed out"})"}});
        try {
            (void)fixture.client.validate_token_online("token");
            FAIL("expected validation to throw");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 504);
            CHECK(std::string(ex.what()) == "Endpoint request timed out");
        }
    }

    SUBCASE("unhandled server exception (empty 500)")
    {
        client_fixture fixture({http_response{500, {}, ""}});
        try {
            (void)fixture.client.validate_token_online("token");
            FAIL("expected validation to throw");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 500);
            CHECK(std::string(ex.what()) == "Moonbase API request failed with HTTP 500");
        }
    }

    SUBCASE("an error page that mentions expiry is not an expired license")
    {
        client_fixture fixture({http_response{503, {}, "Upstream session expired"}});
        try {
            (void)fixture.client.validate_token_online("token");
            FAIL("expected validation to throw");
        } catch (const license_expired_error&) {
            FAIL("only a 400 can say a license expired");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 503);
        }
    }
}

TEST_CASE("a rate-limited request carries the server's Retry-After")
{
    for (const char* header : {"Retry-After", "retry-after"}) {
        CAPTURE(header);
        client_fixture fixture({http_response{429, {{header, "60"}}, ""}});
        try {
            (void)fixture.client.request_activation();
            FAIL("expected the request to throw");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 429);
            REQUIRE(ex.retry_after().has_value());
            CHECK(*ex.retry_after() == std::chrono::seconds{60});
        }
    }

    SUBCASE("an HTTP-date or garbage is ignored")
    {
        for (const char* value : {"Wed, 21 Oct 2026 07:28:00 GMT", "-1", "soon", "99999999999999999999"}) {
            CAPTURE(value);
            client_fixture fixture({http_response{429, {{"Retry-After", value}}, ""}});
            try {
                (void)fixture.client.request_activation();
                FAIL("expected the request to throw");
            } catch (const api_error& ex) {
                CHECK_FALSE(ex.retry_after().has_value());
            }
        }
    }
}

TEST_CASE("request_activation stands in for a blank device name")
{
    client_fixture fixture({
        http_response{200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/r","browser":"https://demo.moonbase.sh/b"})"},
    });
    const license_client client(
        fixture.make_options(),
        std::make_shared<static_device_id_resolver>("  ", "device-id"),
        fixture.validator,
        fixture.transport);

    (void)client.request_activation();

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto body = nlohmann::json::parse(fixture.transport->requests[0].body);
    CHECK(body.at("deviceName") == "Unknown device");
    CHECK(body.at("deviceSignature") == "device-id");
}

TEST_CASE("request_activation refuses an empty device id without contacting the API")
{
    client_fixture fixture({});
    const license_client client(
        fixture.make_options(),
        std::make_shared<static_device_id_resolver>("Test Device", ""),
        fixture.validator,
        fixture.transport);

    CHECK_THROWS_AS((void)client.request_activation(), configuration_error);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("inventory failures are api_error with the status, never a license verdict")
{
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";

    SUBCASE("bad version (400)")
    {
        auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
            http_response{400, {}, R"({"title":"Invalid version","detail":"Version 'x' is not a valid semver in the form 'MAJOR.MINOR.PATCH'","status":400})"},
        });
        const inventory_client inventory(options, transport);
        try {
            (void)inventory.get_release("x", "token");
            FAIL("expected get_release to throw");
        } catch (const license_invalid_error&) {
            FAIL("the inventory endpoints never refuse the license");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 400);
            CHECK(ex.title() == "Invalid version");
        }
    }

    SUBCASE("no access (403)")
    {
        auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
            http_response{403, {}, R"({"title":"No access","detail":"User does not have access to download this product","status":403})"},
        });
        const inventory_client inventory(options, transport);
        try {
            (void)inventory.get_download_url("Mac", "token");
            FAIL("expected get_download_url to throw");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 403);
            CHECK(std::string(ex.what()) == "User does not have access to download this product");
        }
    }
}

// ---------------------------------------------------------------------------
// The activation poll: 404s, and Retry-After.
// ---------------------------------------------------------------------------

TEST_CASE("the activation poll keeps waiting through a 404, whoever sent it")
{
    // A fresh request can 404 on an eventually consistent read, and a proxy's 404
    // cannot be told apart from Moonbase's, so neither may end the activation.
    for (const std::string body : {std::string(api_not_found), std::string{}, std::string{"<html>Not Found</html>"}}) {
        CAPTURE(body);
        client_fixture fixture({http_response{404, {}, body}});
        CHECK_FALSE(fixture.client.get_requested_activation(poll_request()).has_value());
        CHECK(fixture.transport->requests.size() == 1);
    }
}

TEST_CASE("a Retry-After holds back every activation poll until it passes")
{
    detail::activation_poll_gate gate;
    const std::string first = "https://demo.moonbase.sh/api/client/activations/request-123";
    const std::string second = "https://demo.moonbase.sh/api/client/activations/request-456";
    const auto start = detail::activation_poll_gate::clock::now();

    gate.back_off(std::chrono::seconds{60}, start);
    CHECK_FALSE(gate.try_claim(first, start + std::chrono::seconds{59}));
    CHECK_FALSE(gate.try_claim(second, start + std::chrono::seconds{59}));
    CHECK(gate.try_claim(first, start + std::chrono::seconds{60}));
}

TEST_CASE("an outsized Retry-After is capped")
{
    detail::activation_poll_gate gate;
    const std::string url = "https://demo.moonbase.sh/api/client/activations/request-123";
    const auto start = detail::activation_poll_gate::clock::now();

    gate.back_off(std::chrono::hours{24}, start);
    CHECK_FALSE(gate.try_claim(url, start + detail::activation_poll_max_backoff - std::chrono::seconds{1}));
    CHECK(gate.try_claim(url, start + detail::activation_poll_max_backoff));
}

TEST_CASE("a rate-limited activation poll is retryable and backs off")
{
    client_fixture fixture({http_response{429, {{"Retry-After", "60"}}, ""}});
    const auto request = poll_request();

    try {
        (void)fixture.client.get_requested_activation(request);
        FAIL("expected the poll to throw");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == 429);
        CHECK(ex.retry_after() == std::optional<std::chrono::seconds>{std::chrono::seconds{60}});
    }

    // Past the per-request floor, but not the server's Retry-After: the poll
    // answers "not yet" without going out. Had it, the empty queue would throw.
    std::this_thread::sleep_for(detail::activation_poll_min_interval + std::chrono::milliseconds(50));
    CHECK_FALSE(fixture.client.get_requested_activation(request).has_value());
    CHECK(fixture.transport->requests.size() == 1);
}

// ---------------------------------------------------------------------------
// A lapsed license, and a closed store.
// ---------------------------------------------------------------------------

TEST_CASE("validate_token_online reports a lapsed license as expired")
{
    client_fixture fixture({
        http_response{400, {}, R"({"title":"Invalid state","detail":"License has expired","status":400,"errorType":"LicenseExpired"})"},
    });
    CHECK_THROWS_WITH_AS(
        (void)fixture.client.validate_token_online("token"),
        "License has expired",
        license_expired_error);
}

namespace {

// What ClosedTenantMiddleware answers every request to a closed store with.
const http_response store_closed{
    410,
    {},
    R"({"type":"https://tools.ietf.org/html/rfc9110#section-15.5.11","title":"Store closed","status":410,"detail":"This store has closed.","errorType":"StoreClosed"})"};

} // namespace

TEST_CASE("a closed store is a definitive answer on every endpoint")
{
    const auto expect_store_closed = [](const std::function<void()>& call) {
        try {
            call();
            FAIL("expected the call to throw");
        } catch (const store_closed_error& ex) {
            CHECK(std::string(ex.what()) == "This store has closed.");
            CHECK(ex.type() == error_type::store_closed);
            // A license_invalid_error, so grace and existing catch sites treat it as final.
            CHECK(dynamic_cast<const license_invalid_error*>(&ex) != nullptr);
        }
    };

    SUBCASE("request_activation")
    {
        client_fixture fixture({store_closed});
        expect_store_closed([&] { (void)fixture.client.request_activation(); });
    }

    SUBCASE("activation poll")
    {
        // Must end the poll, not read as a retryable failure to keep polling through.
        client_fixture fixture({store_closed});
        expect_store_closed([&] { (void)fixture.client.get_requested_activation(poll_request()); });
    }

    SUBCASE("validate")
    {
        client_fixture fixture({store_closed});
        expect_store_closed([&] { (void)fixture.client.validate_token_online("token"); });
    }

    SUBCASE("revoke")
    {
        client_fixture fixture({store_closed});
        expect_store_closed([&] { fixture.client.revoke_activation("token"); });
    }

    SUBCASE("inventory")
    {
        licensing_options options;
        options.endpoint = "https://demo.moonbase.sh";
        options.product_id = "demo-app";
        auto transport = std::make_shared<moonbase::tests::recording_transport>(
            std::deque<http_response>{store_closed, store_closed});
        const inventory_client inventory(options, transport);
        expect_store_closed([&] { (void)inventory.get_release("", "token"); });
        expect_store_closed([&] { (void)inventory.get_download_url("Mac", "token"); });
    }
}

TEST_CASE("a 410 that is not the API's StoreClosed stays retryable")
{
    client_fixture fixture({http_response{410, {}, "<html><body>Gone</body></html>"}});
    try {
        (void)fixture.client.validate_token_online("token");
        FAIL("expected validation to throw");
    } catch (const license_invalid_error&) {
        FAIL("only the API's own StoreClosed answer is final");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == 410);
    }
}
