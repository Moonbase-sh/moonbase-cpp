#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>

#include "moonbase/store.hpp"

using namespace moonbase;

namespace {

license sample_license()
{
    license value;
    value.id = "license-123";
    value.activation_id = "activation-123";
    value.trial = false;
    value.method = activation_method::online;
    value.licensed_product = product{"demo-app", "Demo Product", "1.2.3", {{"tier", "pro"}}};
    value.issued_to = user{"user-123", "Jane Developer", "jane@example.com", {{"company", "Acme"}}};
    value.issued_at = detail::parse_iso8601_utc("2026-05-08T12:00:00Z");
    value.expires_at = detail::parse_iso8601_utc("2026-06-08T12:00:00Z");
    value.validated_at = detail::parse_iso8601_utc("2026-05-08T12:30:00Z");
    value.owned_sub_product_ids = {"demo-app-pro"};
    value.subscription_id = "subscription-123";
    value.properties = {{"seats", 3}};
    value.token = "jwt";
    return value;
}

} // namespace

TEST_CASE("memory_license_store round-trips and deletes")
{
    memory_license_store store;
    CHECK_FALSE(store.load_local_license().has_value());

    store.store_local_license(sample_license());
    auto loaded = store.load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-123");
    CHECK(loaded->licensed_product.properties.at("tier") == "pro");

    store.delete_local_license();
    CHECK_FALSE(store.load_local_license().has_value());
}

TEST_CASE("file_license_store round-trips and deletes")
{
    const auto path = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-license-test-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".json");

    file_license_store store(path);
    CHECK_FALSE(store.load_local_license().has_value());

    store.store_local_license(sample_license());
    auto loaded = store.load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-123");
    REQUIRE(loaded->expires_at.has_value());
    CHECK(detail::format_iso8601_utc(*loaded->expires_at) == "2026-06-08T12:00:00Z");

    store.delete_local_license();
    CHECK_FALSE(std::filesystem::exists(path));
}
