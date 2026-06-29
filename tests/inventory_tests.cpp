#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <memory>
#include <tuple>

#include "moonbase/errors.hpp"
#include "moonbase/inventory.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

licensing_options make_options()
{
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh/";
    options.product_id = "demo-app";
    options.target_platform = platform::mac;
    options.application_version = "2.3.1";
    options.client_info = "moonbase-juce/9.9 (JUCE v8; TestOS)";
    options.http_connect_timeout = std::chrono::milliseconds{1234};
    options.http_request_timeout = std::chrono::milliseconds{5678};
    return options;
}

} // namespace

TEST_CASE("get_release queries the product endpoint with the license token")
{
    auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
        http_response{200, {},
            R"({"id":"demo-app","name":"Solstice","version":"2.4.0",)"
            R"("releaseDescription":"Line one\nLine two",)"
            R"("downloads":[{"name":"Solstice-2.4.0.dmg","platform":"Mac"}]})"},
    });
    inventory_client client(make_options(), transport);

    const auto info = client.get_release("2.4.0", "the-token");
    CHECK(info.version == "2.4.0");
    CHECK(info.description == "Line one\nLine two");
    CHECK(info.has_downloads);

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.method == "GET");
    CHECK(request.url.find("https://demo.moonbase.sh/api/customer/inventory/products/demo-app?") == 0);
    CHECK(request.url.find("version=2.4.0") != std::string::npos);
    CHECK(request.url.find("includeManifests=false") != std::string::npos);
    CHECK(request.headers.at("Authorization") == "LicenseToken the-token");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");
    CHECK(request.connect_timeout == std::chrono::milliseconds{1234});
    CHECK(request.request_timeout == std::chrono::milliseconds{5678});
}

TEST_CASE("get_release parses the download access-control flags")
{
    auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
        http_response{200, {},
            R"({"id":"demo-app","name":"Solstice","version":"2.4.0",)"
            R"("downloadsNeedsUser":true,"downloadsNeedsOwnership":true,)"
            R"("downloads":[{"name":"Solstice-2.4.0.dmg","platform":"Mac"}]})"},
    });
    inventory_client client(make_options(), transport);

    const auto info = client.get_release("2.4.0", "tok");
    CHECK(info.downloads_need_user);
    CHECK(info.downloads_need_ownership);
}

TEST_CASE("can_download enforces the access-control flags against the license")
{
    const auto make_license = [](bool trial, std::string userId) {
        license lic;
        lic.trial = trial;
        lic.issued_to.id = std::move(userId);
        return lic;
    };
    const auto full = make_license(false, "user-1");
    const auto trial_owned = make_license(true, "user-1");
    const auto trial_anon = make_license(true, "00000000-0000-0000-0000-000000000000");

    release_info anon;       // AllowAnonymous
    release_info customers;  customers.downloads_need_user = true;            // AllowCustomers
    release_info owners;     owners.downloads_need_user = true;               // AllowOwners
                             owners.downloads_need_ownership = true;

    // Anonymous access: everyone can download.
    CHECK(can_download(full, anon));
    CHECK(can_download(trial_owned, anon));
    CHECK(can_download(trial_anon, anon));

    // Needs a user: a full license and an owned trial pass; an anonymous trial doesn't.
    CHECK(can_download(full, customers));
    CHECK(can_download(trial_owned, customers));
    CHECK_FALSE(can_download(trial_anon, customers));

    // Needs ownership: only the full (non-trial) license passes.
    CHECK(can_download(full, owners));
    CHECK_FALSE(can_download(trial_owned, owners));
    CHECK_FALSE(can_download(trial_anon, owners));
}

TEST_CASE("get_release tolerates a release with no notes or downloads")
{
    auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
        http_response{200, {}, R"({"id":"demo-app","name":"Solstice","currentVersion":"2.4.0"})"},
    });
    inventory_client client(make_options(), transport);

    const auto info = client.get_release("", "tok");
    CHECK(info.version == "2.4.0"); // falls back to currentVersion
    CHECK(info.description.empty());
    CHECK_FALSE(info.has_downloads);

    // An empty version omits the query param.
    CHECK(transport->requests.front().url.find("version=") == std::string::npos);
}

TEST_CASE("get_download_url resolves the latest installer to a presigned URL + filename")
{
    auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
        http_response{200, {},
            R"({"location":"https://s3.example.com/bucket/key?X-Amz-Signature=abc)"
            R"(&response-content-disposition=attachment%3B%20filename%3D%22Solstice-2.4.0.dmg%22"})"},
    });
    inventory_client client(make_options(), transport);

    const auto target = client.get_download_url("Mac", "the-token");
    CHECK(target.url.find("s3.example.com") != std::string::npos);
    CHECK(target.filename == "Solstice-2.4.0.dmg");

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.method == "GET");
    CHECK(request.url.find(
              "https://demo.moonbase.sh/api/customer/inventory/products/demo-app/download/Mac/latest?")
          == 0);
    CHECK(request.url.find("redirect=false") != std::string::npos);
    CHECK(request.headers.at("Authorization") == "LicenseToken the-token");
}

TEST_CASE("get_download_url throws when there is no installer for the platform")
{
    auto transport = std::make_shared<moonbase::tests::recording_transport>(std::deque<http_response>{
        http_response{404, {}, R"({"title":"No download for the given platform found"})"},
    });
    inventory_client client(make_options(), transport);

    CHECK_THROWS_AS(std::ignore = client.get_download_url("Linux", "tok"), moonbase::api_error);
}
