#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

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
    value.licensed_product = product{
        "demo-app", "Demo Product", "1.2.3",
        {{"tier", "pro"}, {"regions", {"us", "eu"}}}};
    value.issued_to = user{
        "user-123", "Jane Developer", "jane@example.com",
        {{"company", "Acme"}, {"roles", {"admin", "ops"}}}};
    value.issued_at = detail::parse_iso8601_utc("2026-05-08T12:00:00Z");
    value.expires_at = detail::parse_iso8601_utc("2026-06-08T12:00:00Z");
    value.validated_at = detail::parse_iso8601_utc("2026-05-08T12:30:00Z");
    value.owned_sub_product_ids = {"demo-app-pro"};
    value.subscription_id = "subscription-123";
    value.seat_count = 5;
    value.seats_used = 2;
    value.properties = {{"seats", 3}, {"features", {"export", "sso"}}};
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
    REQUIRE(loaded->issued_to.properties.at("roles").is_array());
    CHECK(loaded->issued_to.properties.at("roles").size() == 2);
    CHECK(loaded->issued_to.properties.at("roles").at(0) == "admin");
    REQUIRE(loaded->properties.at("features").is_array());
    CHECK(loaded->properties.at("features").at(1) == "sso");
    REQUIRE(loaded->seat_count.has_value());
    CHECK(*loaded->seat_count == 5);
    REQUIRE(loaded->seats_used.has_value());
    CHECK(*loaded->seats_used == 2);

    store.delete_local_license();
    CHECK_FALSE(store.load_local_license().has_value());
}

TEST_CASE("a license without seat data round-trips to no seat data")
{
    memory_license_store store;
    auto value = sample_license();
    value.seat_count.reset();
    value.seats_used.reset();
    store.store_local_license(value);

    auto loaded = store.load_local_license();
    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->seat_count.has_value());
    CHECK_FALSE(loaded->seats_used.has_value());
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
    REQUIRE(loaded->licensed_product.properties.at("regions").is_array());
    CHECK(loaded->licensed_product.properties.at("regions").size() == 2);
    REQUIRE(loaded->properties.at("features").is_array());
    CHECK(loaded->properties.at("features").at(0) == "export");
    REQUIRE(loaded->seat_count.has_value());
    CHECK(*loaded->seat_count == 5);
    REQUIRE(loaded->seats_used.has_value());
    CHECK(*loaded->seats_used == 2);

    store.delete_local_license();
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("file_license_store::lock_for_update serializes concurrent acquirers")
{
    const auto path = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-lock-test-" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
         + ".lock");

    // Two independent file_license_store instances pointed at the same path —
    // models two plugin instances (possibly in separate processes) coordinating
    // on the same on-disk license file.
    constexpr int thread_count = 4;
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> worker_exceptions{0};

    auto runner = [&] {
        try {
            file_license_store store(path);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto guard = store.lock_for_update();
            REQUIRE(guard != nullptr);

            const int now_inside = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = max_inside.load(std::memory_order_relaxed);
            while (now_inside > prev
                   && !max_inside.compare_exchange_weak(prev, now_inside,
                                                        std::memory_order_acq_rel)) {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            inside.fetch_sub(1, std::memory_order_acq_rel);
        } catch (...) {
            worker_exceptions.fetch_add(1, std::memory_order_acq_rel);
            ready.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(runner);
    }
    while (ready.load() < thread_count) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    CHECK(worker_exceptions.load() == 0);
    CHECK(max_inside.load() == 1);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto lock_path = path;
    lock_path += ".lock";
    std::filesystem::remove(lock_path, ec);
}

TEST_CASE("file_license_store::lock_for_update survives delete_local_license")
{
    // Regression: when the lock was held on the license file itself,
    // delete_local_license() would unlink the path and orphan the POSIX
    // flock on the dead inode. A sibling acquirer could then open a fresh
    // file at the same path and take an independent lock — letting a
    // refresh persist after the user's clear/revoke. With the sidecar
    // (<path>.lock), the lock target outlives the license payload.
    const auto path = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-lock-survive-" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
         + ".json");

    // Seed once so delete_local_license has something to remove.
    {
        file_license_store seed(path);
        seed.store_local_license(sample_license());
    }

    constexpr int thread_count = 4;
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> worker_exceptions{0};

    auto runner = [&] {
        try {
            file_license_store store(path);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto guard = store.lock_for_update();
            REQUIRE(guard != nullptr);

            const int now_inside = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = max_inside.load(std::memory_order_relaxed);
            while (now_inside > prev
                   && !max_inside.compare_exchange_weak(prev, now_inside,
                                                        std::memory_order_acq_rel)) {
            }

            // Mutate the license file while holding the lock — alternating
            // delete and rewrite hammers exactly the scenario where the old
            // (license-on-itself) lock would have failed.
            try {
                store.delete_local_license();
            } catch (const storage_error&) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            try {
                store.store_local_license(sample_license());
            } catch (const storage_error&) {
            }

            inside.fetch_sub(1, std::memory_order_acq_rel);
        } catch (...) {
            worker_exceptions.fetch_add(1, std::memory_order_acq_rel);
            ready.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(runner);
    }
    while (ready.load() < thread_count) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    CHECK(worker_exceptions.load() == 0);
    CHECK(max_inside.load() == 1);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto lock_path = path;
    lock_path += ".lock";
    std::filesystem::remove(lock_path, ec);
}

TEST_CASE("memory_license_store::lock_for_update blocks concurrent mutations")
{
    // Regression: the default store used to return nullptr from
    // lock_for_update, which meant clearLicense() in a bridge could delete
    // between validate_token_online's should_persist (returning true) and
    // its actual store_local_license, resurrecting the cleared license in
    // memory.
    memory_license_store store;
    store.store_local_license(sample_license());

    std::atomic<bool> holder_inside{false};
    std::atomic<bool> mutator_finished{false};
    std::atomic<bool> mutator_started_while_holder_inside{false};

    std::thread holder([&] {
        auto guard = store.lock_for_update();
        REQUIRE(guard != nullptr);
        holder_inside.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        // If lock_for_update truly serializes against mutating ops, the
        // mutator thread can not have finished its delete by now.
        mutator_started_while_holder_inside.store(
            !mutator_finished.load(std::memory_order_acquire),
            std::memory_order_release);
        holder_inside.store(false, std::memory_order_release);
    });

    std::thread mutator([&] {
        while (!holder_inside.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Holder is inside the lock; this delete must block until the
        // holder releases the recursive_mutex.
        store.delete_local_license();
        mutator_finished.store(true, std::memory_order_release);
    });

    holder.join();
    mutator.join();

    CHECK(mutator_finished.load());
    CHECK(mutator_started_while_holder_inside.load());
    CHECK_FALSE(store.load_local_license().has_value());
}

TEST_CASE("memory_license_store guard allows re-entrant load/store on the holding thread")
{
    // validate_token_online holds lock_for_update across load_local_license
    // and (later) store_local_license. Those must not deadlock when the
    // store's coordination primitive happens to also guard load/store/delete.
    memory_license_store store;

    auto guard = store.lock_for_update();
    REQUIRE(guard != nullptr);

    CHECK_FALSE(store.load_local_license().has_value());
    store.store_local_license(sample_license());
    auto loaded = store.load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-123");
    store.delete_local_license();
    CHECK_FALSE(store.load_local_license().has_value());
}
