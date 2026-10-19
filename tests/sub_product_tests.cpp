#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "moonbase/client.hpp"
#include "moonbase/detail/time.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/store.hpp"
#include "moonbase/validator.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

licensing_options sub_product_options(const std::string& public_key)
{
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh/";
    options.product_id = "demo-app";
    options.public_key = public_key;
    options.account_id = "tenant-1";
    options.target_platform = platform::mac;
    options.application_version = "1.2.3";
    options.metadata = {{"channel", "test"}};
    return options;
}

struct sub_product_fixture {
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    std::shared_ptr<static_device_id_resolver> device_ids =
        std::make_shared<static_device_id_resolver>("Test Device", "device-id");
    std::shared_ptr<moonbase::tests::recording_transport> transport =
        std::make_shared<moonbase::tests::recording_transport>();

    [[nodiscard]] licensing_options options() const { return sub_product_options(key.public_pem); }

    [[nodiscard]] std::string token(const nlohmann::json& extra = nlohmann::json::object()) const
    {
        auto claims = moonbase::tests::default_claims();
        for (const auto& [name, value] : extra.items()) {
            claims[name] = value;
        }
        return moonbase::tests::make_token(key.key.get(), claims);
    }

    [[nodiscard]] license_validator validator() const { return license_validator(options(), device_ids); }

    [[nodiscard]] license_client client(licensing_options with = {}) const
    {
        if (with.public_key.empty()) {
            with = options();
        }
        return license_client(with, device_ids, std::make_shared<license_validator>(with, device_ids), transport);
    }

    void respond(http_response response) { transport->responses.push_back(std::move(response)); }
};

http_response problem(int status, const std::string& error_type, const std::string& detail = "Refused")
{
    nlohmann::json body{{"title", "Refused"}, {"detail", detail}};
    if (!error_type.empty()) {
        body["errorType"] = error_type;
    }
    return http_response{status, {}, body.dump()};
}

std::string query_of(const std::string& url)
{
    const auto at = url.find('?');
    return at == std::string::npos ? std::string() : url.substr(at + 1);
}

const nlohmann::json sample_catalog = nlohmann::json::parse(R"({
    "currency": "EUR",
    "subProducts": [
        {
            "id": "strings",
            "product": {
                "id": "demo-app.strings",
                "name": "Strings",
                "tagline": "Bowed and plucked",
                "description": null,
                "website": null,
                "iconUrl": "https://cdn.example.com/strings.png",
                "owned": false,
                "subscribed": false,
                "defaultVariation": {"id": "one-off", "name": "Strings", "originalPrice": {"EUR": 49}, "price": {"EUR": 39.5, "USD": 42}, "hasDiscount": true, "recurrence": {"type": "OneOff"}},
                "variations": [
                    {"id": "one-off", "name": "Strings", "originalPrice": {"EUR": 49}, "price": {"EUR": 39.5}, "hasDiscount": true, "recurrence": {"type": "OneOff"}},
                    {"id": "monthly", "name": "Strings monthly", "originalPrice": {"EUR": 5}, "price": {"EUR": 5}, "hasDiscount": false, "recurrence": {"type": "Recurring", "cycleLength": "Monthly"}}
                ]
            },
            "includedInLicense": true,
            "active": false,
            "revoked": false,
            "trial": {"status": "Available", "numberOfDays": 7, "requiresNewsletterOptIn": false},
            "buyUrl": "https://demo.moonbase.sh/buy/demo-app.strings"
        },
        {
            "id": "pack",
            "product": {"id": "demo-app.pack", "name": "Pack", "tagline": "", "owned": true, "subscribed": false},
            "includedInLicense": true,
            "active": true,
            "expiresAt": "2027-01-01T00:00:00Z",
            "seats": {"total": 2, "used": 1, "remaining": 1},
            "trial": {"status": "SomethingNew", "requiresNewsletterOptIn": false}
        },
        {
            "id": "keys",
            "product": {"id": "demo-app.keys", "name": "Keys", "tagline": "", "owned": false, "subscribed": false},
            "includedInLicense": true,
            "active": true,
            "revoked": false,
            "expiresAt": "2027-02-01T12:30:00.1234567",
            "trial": {"status": "Active", "numberOfDays": 14, "expiresAt": "2027-02-01T14:30:00+02:00", "requiresNewsletterOptIn": false}
        }
    ]
})");

} // namespace

static_assert(!std::is_base_of_v<license_invalid_error, no_eligible_license_error>,
              "no seat of an add-on says nothing against the license");

//==============================================================================
// The license token's add-ons

TEST_CASE("the license reads the add-ons its token reports")
{
    sub_product_fixture fixture;
    const auto trial_ends = moonbase::tests::now_seconds() + 3600;
    const auto token = fixture.token({{"sp", {
        {{"id", "pack"}, {"active", true}, {"trial", false}, {"seats", {{"total", 3}, {"used", 1}, {"remaining", 2}}}},
        {{"id", "strings"}, {"active", true}, {"trial", true}, {"exp", trial_ends}},
        {{"id", "brass"}, {"active", false}, {"trial", false}, {"seats", {{"total", 1}, {"used", 1}, {"remaining", 0}}}},
        {{"id", "keys"}, {"active", false}, {"trial", false}, {"revoked", true}},
    }}});

    const auto value = fixture.validator().validate_token(token);

    REQUIRE(value.sub_products.size() == 4);
    const auto& pack = value.sub_products[0];
    CHECK(pack.id == "pack");
    CHECK(pack.active);
    CHECK_FALSE(pack.trial);
    CHECK_FALSE(pack.revoked);
    CHECK_FALSE(pack.expires_at.has_value());
    REQUIRE(pack.seats.has_value());
    CHECK(pack.seats->total == 3);
    CHECK(pack.seats->used == 1);
    CHECK(pack.seats->remaining == 2);

    const auto& strings = value.sub_products[1];
    CHECK(strings.trial);
    REQUIRE(strings.expires_at.has_value());
    CHECK(detail::to_epoch_seconds(*strings.expires_at) == trial_ends);
    CHECK_FALSE(strings.seats.has_value());

    CHECK(value.sub_products[3].revoked);

    CHECK(has_sub_product(value, "pack"));
    CHECK(has_sub_product(value, "demo-app.strings"));
    CHECK_FALSE(has_sub_product(value, "brass"));   // owned, but every seat is taken elsewhere
    CHECK_FALSE(has_sub_product(value, "keys"));    // revoked on this device
    CHECK_FALSE(has_sub_product(value, "unknown"));
    // The ownership list is unchanged and separate.
    CHECK(value.owned_sub_product_ids == std::vector<std::string>{"demo-app-pro", "demo-app-extra"});
}

TEST_CASE("an add-on whose trial ran out since the license was issued does not unlock")
{
    sub_product_fixture fixture;
    const auto ended = moonbase::tests::now_seconds() - 60;
    const auto value = fixture.validator().validate_token(fixture.token({{"sp", {
        {{"id", "strings"}, {"active", true}, {"trial", true}, {"exp", ended}},
    }}}));

    CHECK_FALSE(has_sub_product(value, "strings"));
    // As of a moment before it ended, it did.
    CHECK(has_sub_product(value, "strings", detail::from_epoch_seconds(ended - 1)));
}

TEST_CASE("an add-on entry the SDK cannot read is skipped, not the license")
{
    sub_product_fixture fixture;
    const auto value = fixture.validator().validate_token(fixture.token({{"sp", {
        {{"active", true}},
        "pack",
        {{"id", 5}, {"active", true}},
        {{"id", "strings"}, {"active", true}, {"trial", "yes"}, {"future", {{"field", 1}}}, {"seats", {{"total", 2}}}},
    }}}));

    REQUIRE(value.sub_products.size() == 1);
    CHECK(value.sub_products[0].id == "strings");
    CHECK(value.sub_products[0].active);
    CHECK_FALSE(value.sub_products[0].trial);         // not a boolean
    CHECK_FALSE(value.sub_products[0].seats.has_value()); // incomplete
    CHECK(has_sub_product(value, "strings"));
}

TEST_CASE("a license without add-ons reports none")
{
    sub_product_fixture fixture;

    SUBCASE("no claim")
    {
        const auto value = fixture.validator().validate_token(fixture.token());
        CHECK(value.sub_products.empty());
        CHECK_FALSE(has_sub_product(value, "pack"));
    }

    SUBCASE("a claim that is not a list")
    {
        const auto value = fixture.validator().validate_token(fixture.token({{"sp", {{"id", "pack"}}}}));
        CHECK(value.sub_products.empty());
    }
}

TEST_CASE("find_sub_product names an add-on by its own id or its whole id")
{
    license value;
    value.licensed_product.id = "demo-app";
    value.sub_products.push_back(sub_product{"strings", true, false, false, std::nullopt, std::nullopt});

    REQUIRE(find_sub_product(value, "strings") != nullptr);
    CHECK(find_sub_product(value, "demo-app.strings") == &value.sub_products[0]);
    // Another product's add-on of the same name is not this one.
    CHECK(find_sub_product(value, "other-app.strings") == nullptr);
    CHECK(find_sub_product(value, "demo-app.") == nullptr);
    CHECK(find_sub_product(value, "") == nullptr);
}

TEST_CASE("a stored license keeps its add-ons")
{
    license value;
    value.id = "license-123";
    value.activation_id = "activation-123";
    value.licensed_product = {"demo-app", "Demo App", std::nullopt, nlohmann::json::object()};
    value.issued_to = {"user-123", "Jane", "jane@example.com", nlohmann::json::object()};
    value.token = "token";
    value.sub_products.push_back(sub_product{"pack", true, false, false, std::nullopt, sub_product_seats{3, 1, 2}});
    value.sub_products.push_back(
        sub_product{"strings", true, true, false, detail::from_epoch_seconds(1798761600), std::nullopt});
    value.sub_products.push_back(sub_product{"keys", false, false, true, std::nullopt, std::nullopt});

    // Through JSON, as a file store writes and reads it.
    const nlohmann::json json = value;
    const auto loaded = std::optional<license>(json.get<license>());

    REQUIRE(loaded->sub_products.size() == 3);
    REQUIRE(loaded->sub_products[0].seats.has_value());
    CHECK(loaded->sub_products[0].seats->remaining == 2);
    CHECK(loaded->sub_products[1].trial);
    REQUIRE(loaded->sub_products[1].expires_at.has_value());
    CHECK(detail::to_epoch_seconds(*loaded->sub_products[1].expires_at) == 1798761600);
    CHECK(loaded->sub_products[2].revoked);
    CHECK_FALSE(loaded->sub_products[2].active);
}

TEST_CASE("a stored license reads without add-ons it can't make sense of")
{
    license value;
    value.id = "license-123";
    value.activation_id = "activation-123";
    value.licensed_product = {"demo-app", "Demo App", std::nullopt, nlohmann::json::object()};
    value.issued_to = {"user-123", "Jane", "jane@example.com", nlohmann::json::object()};
    value.token = "token";
    nlohmann::json json = value;

    SUBCASE("stored before add-ons were reported")
    {
        json.erase("subProducts");
        CHECK(json.get<license>().sub_products.empty());
    }

    SUBCASE("an entry that can't be read is dropped, the license kept")
    {
        json["subProducts"] = nlohmann::json::array({
            nlohmann::json{{"active", true}},
            nlohmann::json{{"id", "pack"}, {"active", true}, {"expiresAt", "not a time"}},
            nlohmann::json{{"id", "strings"}, {"active", true}},
        });
        const auto loaded = json.get<license>();
        REQUIRE(loaded.sub_products.size() == 1);
        CHECK(loaded.sub_products[0].id == "strings");
    }
}

//==============================================================================
// The add-on endpoints

TEST_CASE("activate_sub_product posts the license to the add-on's activation")
{
    sub_product_fixture fixture;
    const auto fresh = fixture.token({{"sp", {{{"id", "strings"}, {"active", true}, {"trial", false}}}}});
    fixture.respond(http_response{200, {}, fresh});

    const auto value = fixture.client().activate_sub_product("current-token", "strings");

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto& request = fixture.transport->requests.front();
    CHECK(request.method == "POST");
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/licenses/demo-app/sub-products/strings/activate?") == 0);
    const auto query = query_of(request.url);
    CHECK(query.find("format=JWT") != std::string::npos);
    CHECK(query.find("platform=Mac") != std::string::npos);
    CHECK(query.find("appVersion=1.2.3") != std::string::npos);
    CHECK(query.find("meta%5Bchannel%5D=test") != std::string::npos);
    CHECK(query.find("autoActivateSubProducts") == std::string::npos);
    CHECK(request.body == "current-token");
    CHECK(request.headers.at("Content-Type") == "text/plain");
    CHECK(request.headers.at("x-mb-client") == "moonbase-cpp");

    CHECK(value.token == fresh);
    CHECK(has_sub_product(value, "strings"));
}

TEST_CASE("request_sub_product_trial posts the license to the add-on's trial")
{
    sub_product_fixture fixture;
    const auto fresh = fixture.token({{"sp", {
        {{"id", "strings"}, {"active", true}, {"trial", true}, {"exp", moonbase::tests::now_seconds() + 86400}},
    }}});
    fixture.respond(http_response{200, {}, fresh});

    const auto value = fixture.client().request_sub_product_trial("current-token", "demo-app.strings");

    const auto& request = fixture.transport->requests.at(0);
    CHECK(request.method == "POST");
    // A whole product ID goes in as given; the server takes either form.
    CHECK(request.url.find("https://demo.moonbase.sh/api/client/licenses/demo-app/sub-products/demo-app.strings/trial?") == 0);
    CHECK(request.body == "current-token");
    REQUIRE(value.sub_products.size() == 1);
    CHECK(value.sub_products[0].trial);
}

TEST_CASE("an add-on id is escaped in the path")
{
    sub_product_fixture fixture;
    fixture.respond(http_response{200, {}, fixture.token()});

    (void)fixture.client().activate_sub_product("current-token", "a b/c");

    CHECK(fixture.transport->requests.at(0).url.find("/sub-products/a%20b%2Fc/activate?") != std::string::npos);
}

TEST_CASE("an add-on call without an add-on id is refused without contacting the API")
{
    sub_product_fixture fixture;
    const auto client = fixture.client();

    CHECK_THROWS_AS((void)client.activate_sub_product("current-token", ""), configuration_error);
    CHECK_THROWS_AS((void)client.request_sub_product_trial("current-token", "  "), configuration_error);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("an add-on call that gets no license token back is retryable")
{
    sub_product_fixture fixture;
    fixture.respond(http_response{200, {}, "<html><body>Sign in to the Wi-Fi</body></html>"});

    try {
        (void)fixture.client().activate_sub_product("current-token", "strings");
        FAIL("expected the call to throw");
    } catch (const license_invalid_error&) {
        FAIL("a captive portal says nothing about the license");
    } catch (const api_error& ex) {
        CHECK(ex.status_code() == 200);
    }
}

TEST_CASE("the add-on endpoints tell a refused add-on from a refused license")
{
    const std::vector<std::pair<std::string, std::function<void(const license_client&)>>> endpoints{
        {"activate", [](const license_client& client) { (void)client.activate_sub_product("token", "strings"); }},
        {"trial", [](const license_client& client) { (void)client.request_sub_product_trial("token", "strings"); }},
        {"list", [](const license_client& client) { (void)client.list_sub_products("token"); }},
    };

    for (const auto& endpoint : endpoints) {
        const auto& name = endpoint.first;
        const auto& call = endpoint.second;
        CAPTURE(name);
        sub_product_fixture fixture;
        const auto client = fixture.client();

        // No seat to give: about the add-on, so not a license verdict.
        fixture.respond(problem(400, "NoEligibleLicense", "No license with available activations found"));
        try {
            call(client);
            FAIL("expected no_eligible_license_error");
        } catch (const no_eligible_license_error& ex) {
            CHECK(ex.type() == error_type::no_eligible_license);
            CHECK(std::string(ex.what()) == "No license with available activations found");
        }

        // A refusal without a code is about the add-on too.
        fixture.respond(problem(400, "", "'Strings' is not included in the license for 'Demo App'"));
        try {
            call(client);
            FAIL("expected operation_not_supported_error");
        } catch (const operation_not_supported_error& ex) {
            CHECK(std::string(ex.what()) == "'Strings' is not included in the license for 'Demo App'");
        }

        // The license itself refused.
        for (const auto* code : {"LicenseInvalid", "LicenseRevoked", "LicenseActivationRevoked"}) {
            CAPTURE(code);
            fixture.respond(problem(400, code));
            CHECK_THROWS_AS(call(client), license_invalid_error);
        }
        fixture.respond(problem(400, "LicenseExpired"));
        CHECK_THROWS_AS(call(client), license_expired_error);

        // No verdict at all.
        fixture.respond(problem(404, ""));
        CHECK_THROWS_AS(call(client), api_error);
        fixture.respond(http_response{503, {}, R"({"message":"Service Unavailable"})"});
        CHECK_THROWS_AS(call(client), api_error);
        fixture.respond(problem(410, "StoreClosed"));
        CHECK_THROWS_AS(call(client), store_closed_error);
    }
}

TEST_CASE("auto_activate_sub_products off asks the server not to take add-on seats")
{
    sub_product_fixture fixture;
    auto options = fixture.options();
    options.auto_activate_sub_products = false;
    const auto client = fixture.client(options);

    fixture.respond(http_response{200, {}, fixture.token()});
    (void)client.validate_token_online("current-token");
    fixture.respond(http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/r","browser":"https://demo.moonbase.sh/b"})"});
    (void)client.request_activation();
    fixture.respond(http_response{200, {}, fixture.token()});
    (void)client.activate_sub_product("current-token", "strings");

    REQUIRE(fixture.transport->requests.size() == 3);
    for (const auto& request : fixture.transport->requests) {
        CAPTURE(request.url);
        CHECK(query_of(request.url).find("autoActivateSubProducts=false") != std::string::npos);
    }

    // On, the server's default, it is left out.
    fixture.respond(http_response{200, {}, fixture.token()});
    (void)fixture.client().validate_token_online("current-token");
    CHECK(query_of(fixture.transport->requests.back().url).find("autoActivateSubProducts") == std::string::npos);
}

//==============================================================================
// The add-on list

TEST_CASE("list_sub_products posts the license and reads the add-on list")
{
    sub_product_fixture fixture;
    fixture.respond(http_response{200, {}, sample_catalog.dump()});

    const auto catalog = fixture.client().list_sub_products("current-token");

    const auto& request = fixture.transport->requests.at(0);
    CHECK(request.method == "POST");
    CHECK(request.url == "https://demo.moonbase.sh/api/client/licenses/demo-app/sub-products");
    CHECK(request.body == "current-token");
    CHECK(request.headers.at("Content-Type") == "text/plain");

    CHECK(catalog.currency == "EUR");
    REQUIRE(catalog.sub_products.size() == 3);

    const auto& strings = catalog.sub_products[0];
    CHECK(strings.id == "strings");
    CHECK(strings.product.id == "demo-app.strings");
    CHECK(strings.product.name == "Strings");
    CHECK(strings.product.tagline == "Bowed and plucked");
    CHECK(strings.product.description.empty());
    CHECK(strings.product.icon_url == "https://cdn.example.com/strings.png");
    CHECK(strings.included_in_license);
    CHECK_FALSE(strings.active);
    REQUIRE(strings.product.default_variation.has_value());
    CHECK(strings.product.default_variation->price.at("EUR") == doctest::Approx(39.5));
    CHECK(strings.product.default_variation->price.at("USD") == doctest::Approx(42));
    CHECK(strings.product.default_variation->original_price.at("EUR") == doctest::Approx(49));
    CHECK(strings.product.default_variation->has_discount);
    REQUIRE(strings.product.variations.size() == 2);
    REQUIRE(strings.product.variations[1].recurrence.has_value());
    CHECK(strings.product.variations[1].recurrence->type == "Recurring");
    CHECK(strings.product.variations[1].recurrence->cycle_length == "Monthly");
    CHECK(strings.product.variations[0].recurrence->cycle_length.empty());
    REQUIRE(strings.trial.has_value());
    CHECK(strings.trial->status == sub_product_trial_status::available);
    CHECK(strings.trial->number_of_days == 7);
    CHECK_FALSE(strings.trial->expires_at.has_value());
    CHECK(strings.buy_url == "https://demo.moonbase.sh/buy/demo-app.strings");

    const auto& pack = catalog.sub_products[1];
    CHECK(pack.active);
    CHECK(pack.product.owned);
    CHECK_FALSE(pack.revoked); // absent
    CHECK_FALSE(pack.product.default_variation.has_value());
    CHECK(pack.product.variations.empty());
    CHECK(pack.buy_url.empty());
    REQUIRE(pack.expires_at.has_value());
    CHECK(*pack.expires_at == detail::parse_iso8601_utc("2027-01-01T00:00:00Z"));
    REQUIRE(pack.seats.has_value());
    CHECK(pack.seats->total == 2);
    CHECK(pack.seats->remaining == 1);
    // A status this version does not know.
    CHECK(pack.trial->status == sub_product_trial_status::unavailable);
    CHECK_FALSE(pack.trial->number_of_days.has_value());

    const auto& keys = catalog.sub_products[2];
    CHECK(keys.trial->status == sub_product_trial_status::active);
    // A time without a zone is UTC; one with an offset is moved to UTC.
    CHECK(*keys.expires_at == detail::parse_iso8601_utc("2027-02-01T12:30:00Z"));
    CHECK(*keys.trial->expires_at == detail::parse_iso8601_utc("2027-02-01T12:30:00Z"));
}

TEST_CASE("an answer that isn't the add-on list is retryable")
{
    for (const auto* body : {
             "<html><body>Sign in to the Wi-Fi</body></html>",
             R"({"subProducts":[]})",
             R"({"currency":"EUR","subProducts":[{"id":"strings"}]})",
             R"({"currency":"EUR","subProducts":[{"id":"pack","product":{"id":"demo-app.pack","name":"Pack","owned":true,"subscribed":false},"includedInLicense":true,"active":true,"expiresAt":"next year"}]})",
         }) {
        CAPTURE(body);
        sub_product_fixture fixture;
        fixture.respond(http_response{200, {}, body});
        try {
            (void)fixture.client().list_sub_products("current-token");
            FAIL("expected the list to throw");
        } catch (const api_error& ex) {
            CHECK(ex.status_code() == 200);
            CHECK(std::string(ex.what()).find("Could not parse the add-on list") == 0);
        }
    }
}

TEST_CASE("an empty add-on list is a list")
{
    sub_product_fixture fixture;
    fixture.respond(http_response{200, {}, R"({"currency":"USD","subProducts":[]})"});

    const auto catalog = fixture.client().list_sub_products("current-token");

    CHECK(catalog.currency == "USD");
    CHECK(catalog.sub_products.empty());
}

TEST_CASE("parse_api_timestamp reads the times .NET writes, all as UTC")
{
    const auto utc = detail::parse_iso8601_utc("2027-02-01T12:30:00Z");
    CHECK(detail::parse_api_timestamp("2027-02-01T12:30:00Z") == utc);
    CHECK(detail::parse_api_timestamp("2027-02-01T12:30:00") == utc);
    CHECK(detail::parse_api_timestamp("2027-02-01T12:30:00.1234567") == utc);
    CHECK(detail::parse_api_timestamp("2027-02-01T12:30:00.5Z") == utc);
    CHECK(detail::parse_api_timestamp("2027-02-01T14:30:00+02:00") == utc);
    CHECK(detail::parse_api_timestamp("2027-02-01T07:00:00-05:30") == utc);
    CHECK(detail::parse_api_timestamp("2027-02-01T12:30:00+00:00") == utc);

    for (const auto* bad : {"", "2027-02-01", "2027-02-01T12:30:00+0200", "2027-02-01T12:30:00 UTC",
                            "2027-02-01T12:30:00+24:00", "2027-02-30T12:30:00Z", "next year"}) {
        CAPTURE(bad);
        CHECK_THROWS((void)detail::parse_api_timestamp(bad));
    }
}

//==============================================================================
// The licensing facade

namespace {

struct facade {
    sub_product_fixture fixture;
    std::shared_ptr<memory_license_store> store = std::make_shared<memory_license_store>();
    licensing instance{fixture.options(), store, fixture.device_ids, fixture.transport};
};

} // namespace

TEST_CASE("licensing stores the license an add-on call returns")
{
    facade setup;
    const auto fresh = setup.fixture.token({{"sp", {{{"id", "strings"}, {"active", true}, {"trial", false}}}}});

    SUBCASE("activate_sub_product")
    {
        setup.fixture.respond(http_response{200, {}, fresh});
        const auto value = setup.instance.activate_sub_product(setup.fixture.token(), "strings");
        CHECK(has_sub_product(value, "strings"));
    }

    SUBCASE("request_sub_product_trial")
    {
        setup.fixture.respond(http_response{200, {}, fresh});
        const auto value = setup.instance.request_sub_product_trial(setup.fixture.token(), "strings");
        CHECK(has_sub_product(value, "strings"));
    }

    const auto stored = setup.store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == fresh);
}

TEST_CASE("licensing leaves the store alone when should_persist says no")
{
    facade setup;
    setup.fixture.respond(http_response{200, {}, setup.fixture.token()});

    bool asked = false;
    (void)setup.instance.activate_sub_product(setup.fixture.token(), "strings", [&] {
        asked = true;
        return false;
    });

    CHECK(asked);
    CHECK_FALSE(setup.store->load_local_license().has_value());
}

TEST_CASE("licensing refuses add-on calls from a trial without contacting the API")
{
    facade setup;
    const auto trial = setup.fixture.token({{"trial", true}});

    CHECK_THROWS_AS((void)setup.instance.activate_sub_product(trial, "strings"), operation_not_supported_error);
    CHECK_THROWS_AS((void)setup.instance.request_sub_product_trial(trial, "strings"), operation_not_supported_error);
    CHECK(setup.fixture.transport->requests.empty());
}

TEST_CASE("licensing lists add-ons from a license or a trial")
{
    facade setup;
    setup.fixture.respond(http_response{200, {}, sample_catalog.dump()});
    setup.fixture.respond(http_response{200, {}, sample_catalog.dump()});

    CHECK(setup.instance.list_sub_products(setup.fixture.token()).sub_products.size() == 3);
    CHECK(setup.instance.list_sub_products(setup.fixture.token({{"trial", true}})).sub_products.size() == 3);
    CHECK_FALSE(setup.store->load_local_license().has_value()); // listing changes nothing
}

TEST_CASE("licensing checks the token locally before any add-on call")
{
    facade setup;
    const auto foreign = setup.fixture.token({{"sig", "another-device"}});

    CHECK_THROWS_AS((void)setup.instance.activate_sub_product(foreign, "strings"), license_device_mismatch_error);
    CHECK_THROWS_AS((void)setup.instance.request_sub_product_trial(foreign, "strings"), license_device_mismatch_error);
    CHECK_THROWS_AS((void)setup.instance.list_sub_products(foreign), license_device_mismatch_error);
    CHECK(setup.fixture.transport->requests.empty());
}

namespace {

// A memory store that counts how deep its update lock is held, and a transport
// that notes that depth whenever a request goes out.
class lock_watching_store : public memory_license_store {
public:
    std::atomic<int> depth{0};

    std::unique_ptr<store_lock_guard> lock_for_update() override
    {
        struct held : store_lock_guard {
            held(std::unique_ptr<store_lock_guard> inner, std::atomic<int>& depth)
                : inner_(std::move(inner)), depth_(depth)
            {
                ++depth_;
            }
            ~held() override { --depth_; }
            std::unique_ptr<store_lock_guard> inner_;
            std::atomic<int>& depth_;
        };
        return std::make_unique<held>(memory_license_store::lock_for_update(), depth);
    }
};

class depth_noting_transport : public moonbase::tests::recording_transport {
public:
    explicit depth_noting_transport(std::shared_ptr<lock_watching_store> store) : store_(std::move(store)) {}
    std::vector<int> depth_at_send;

    http_response send(const http_request& request) override
    {
        depth_at_send.push_back(store_->depth.load());
        return recording_transport::send(request);
    }

private:
    std::shared_ptr<lock_watching_store> store_;
};

} // namespace

TEST_CASE("an add-on call holds the store's lock from its request until its answer is stored")
{
    sub_product_fixture fixture;
    auto store = std::make_shared<lock_watching_store>();
    auto transport = std::make_shared<depth_noting_transport>(store);
    licensing instance(fixture.options(), store, fixture.device_ids, transport);
    const auto fresh = fixture.token({{"sp", {{{"id", "drum-kit"}, {"active", true}, {"trial", false}}}}});
    transport->responses.push_back(http_response{200, {}, fresh});

    bool asked_under_lock = false;
    (void)instance.activate_sub_product(fixture.token(), "drum-kit", [&] {
        asked_under_lock = store->depth.load() == 1;
        return true;
    });

    // So another instance's validation, which takes the same lock, can't read
    // the store before this answer is in it, or store an older one after it.
    REQUIRE(transport->depth_at_send.size() == 1);
    CHECK(transport->depth_at_send[0] == 1);
    CHECK(asked_under_lock);
    CHECK(store->depth.load() == 0);
    REQUIRE(store->load_local_license().has_value());
    CHECK(store->load_local_license()->token == fresh);
}

TEST_CASE("a validation that finds another copy of its license validated at the same time asks the server")
{
    // An add-on call answers with the activation's last validation time, so the
    // copy it stored and the one from before it tie, and nothing in them says
    // which is current.
    facade setup;
    // One validation time for both, as the server gives them.
    const auto validated = moonbase::tests::now_seconds() - 30;
    const auto before = setup.fixture.token({{"validated", validated}});
    const auto with_add_on = setup.fixture.token(
        {{"validated", validated}, {"sp", {{{"id", "drum-kit"}, {"active", true}, {"trial", false}}}}});
    auto checked_claims = moonbase::tests::default_claims();
    checked_claims["validated"] = moonbase::tests::now_seconds();
    checked_claims["sp"] = nlohmann::json::array({{{"id", "drum-kit"}, {"active", true}, {"trial", false}}});
    const auto checked = setup.fixture.token(checked_claims);

    SUBCASE("a caller still holding the copy from before the add-on")
    {
        setup.fixture.respond(http_response{200, {}, with_add_on});
        (void)setup.instance.activate_sub_product(before, "drum-kit");

        setup.fixture.respond(http_response{200, {}, checked});
        const auto validated = setup.instance.validate_token_online(before);

        CHECK(validated.token == checked);
        CHECK(has_sub_product(validated, "drum-kit"));
        CHECK(setup.fixture.transport->requests.size() == 2);
    }

    SUBCASE("a caller holding a newer copy than the store, one that wasn't stored")
    {
        setup.store->store_local_license(setup.instance.validate_token_local(before));
        setup.fixture.respond(http_response{200, {}, with_add_on});
        const auto newer = setup.instance.client().activate_sub_product(before, "drum-kit"); // stores nothing

        setup.fixture.respond(http_response{200, {}, checked});
        const auto validated = setup.instance.validate_token_online(newer.token);

        CHECK(has_sub_product(validated, "drum-kit")); // not the store's older copy
        CHECK(setup.fixture.transport->requests.size() == 2);
    }

    REQUIRE(setup.store->load_local_license().has_value());
    CHECK(setup.store->load_local_license()->token == checked);
}

TEST_CASE("a validation that finds the same copy stored stays within the throttle")
{
    facade setup;
    const auto token = setup.fixture.token();
    setup.store->store_local_license(setup.instance.validate_token_local(token));

    const auto validated = setup.instance.validate_token_online(token);

    CHECK(validated.token == token);
    CHECK(setup.fixture.transport->requests.empty());
}

