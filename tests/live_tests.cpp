#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "moonbase/moonbase.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

constexpr const char* demo_public_key = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAutOqeUiPMgYjAwQ53CyKhJSqojr2bejce0CshQi9Hd8mNZbkoROx
oS56eIzehFSlX4YwHnF47AR1+fPOe7Q33Cgzd6d9xqksiMH7sWK2mADIlB66vZdW
uk3Me0UMB22Biy1RQbSRMivu79MxCofsympoL/5CFjJLd1u37kxjuRWVLjJS84Rr
3L2W7R7Exnno/giC+L/Dv711mjgstmtlAQm5ZINvFvoLA1eFTDs6nlCs3dpJSiq3
fsBUMT9FtudzS5As54jeT/8MB66fJJ0A1LQ/v5CW8ACQYseFSIoOKErD3xU7QLIJ
ERUn++6CVMPvZo67jVbTY+GCXYfW4gGVZQIDAQAB
-----END RSA PUBLIC KEY-----)";

} // namespace

TEST_CASE("live API activation flow")
{
    const auto enabled = moonbase::tests::getenv_or("MOONBASE_CPP_LIVE_TESTS", "0");
    if (enabled != "1" && enabled != "true" && enabled != "TRUE") {
        return;
    }

    licensing_options options;
    options.endpoint = moonbase::tests::getenv_or("MOONBASE_CPP_ENDPOINT", "https://demo.moonbase.sh");
    options.product_id = moonbase::tests::getenv_or("MOONBASE_CPP_PRODUCT_ID", "demo-app");
    options.public_key = moonbase::tests::getenv_or("MOONBASE_CPP_PUBLIC_KEY", demo_public_key);
    const auto account_id = moonbase::tests::getenv_or("MOONBASE_CPP_ACCOUNT_ID", "");
    if (!account_id.empty()) {
        options.account_id = account_id;
    }
    options.target_platform = platform::unknown;
    options.application_version = "0.1.0-test";
    options.metadata = {{"suite", "moonbase-cpp"}};

    const auto unique_id = "moonbase-cpp-test-" + std::to_string(moonbase::tests::now_seconds());
    auto fingerprints = std::make_shared<static_fingerprint_provider>("Moonbase C++ Test", unique_id);
    auto transport = std::make_shared<curl_http_transport>();
    licensing sdk(options, nullptr, fingerprints, transport);

    const auto activation = sdk.request_activation();
    CHECK_FALSE(activation.id.empty());
    CHECK_FALSE(activation.request_url.empty());
    CHECK_FALSE(activation.browser_url.empty());

    CHECK_FALSE(sdk.get_requested_activation(activation).has_value());

    http_request fulfill;
    fulfill.method = "POST";
    fulfill.url = detail::trim_trailing_slashes(options.endpoint) +
        "/api/customer/activations/" +
        activation.id +
        "/trial";
    fulfill.headers = {
        {"Accept", "application/json"},
        {"User-Agent", "moonbase-cpp-live-test"},
        {"x-mb-client", "moonbase-cpp"},
    };
    const auto fulfill_response = transport->send(fulfill);
    INFO(fulfill_response.body);
    REQUIRE(fulfill_response.status_code >= 200);
    REQUIRE(fulfill_response.status_code < 300);

    std::optional<license> fulfilled;
    for (int attempt = 0; attempt < 20 && !fulfilled; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        fulfilled = sdk.get_requested_activation(activation);
    }

    REQUIRE(fulfilled.has_value());
    CHECK(fulfilled->trial);
    CHECK(fulfilled->licensed_product.id == options.product_id);
    CHECK_FALSE(fulfilled->token.empty());
}
