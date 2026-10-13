#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
 #include <sys/stat.h>
 #include <unistd.h>
#endif

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

std::string unique_suffix()
{
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
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

TEST_CASE("file_license_store removes an unparseable license file and reports none")
{
    const auto path = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-corrupt-" + unique_suffix() + ".json");
    {
        std::ofstream file(path);
        file << "this is not valid json {{{";
    }
    REQUIRE(std::filesystem::exists(path));

    file_license_store store(path);
    // A corrupt file reads as "no stored license" instead of throwing on every
    // load...
    CHECK_FALSE(store.load_local_license().has_value());
    // ...and is deleted so a fresh activation can be written on top.
    CHECK_FALSE(std::filesystem::exists(path));

    // A new license then round-trips cleanly over the cleared slot.
    store.store_local_license(sample_license());
    auto loaded = store.load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-123");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("file_license_store readers never catch the license half-written")
{
    // Readers don't take the lock (an instance loading its license at startup
    // doesn't), and load_local_license deletes a file it can't parse. So a reader
    // that caught a write half done would delete the license, and the customer
    // would have to activate again. Nor may a reader fail to open it while a
    // write lands, which Windows refuses for a moment after the rename.
    const auto dir = std::filesystem::temp_directory_path() / ("moonbase-cpp-torn-" + unique_suffix());
    std::filesystem::create_directories(dir);
    const auto path = dir / "license.mb";

    auto small = sample_license();
    auto large = sample_license();
    large.token = std::string(64 * 1024, 'x'); // takes several writes, so a wide window

    file_license_store writer(path);
    writer.store_local_license(small);

    std::atomic<bool> done{false};
    std::atomic<int> reads{0};
    std::atomic<int> misses{0};
    std::thread reading([&] {
        file_license_store reader(path);
        while (!done.load()) {
            try {
                if (!reader.load_local_license()) {
                    ++misses;
                }
            } catch (const storage_error&) {
                ++misses;
            }
            ++reads;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    for (int i = 0; reads.load() < 100 && i < 100000; ++i) {
        writer.store_local_license(i % 2 == 0 ? large : small);
    }
    done = true;
    reading.join();

    CHECK(reads.load() > 0);
    CHECK(misses.load() == 0);
    CHECK(std::filesystem::exists(path));

    // Nothing left beside it but the license itself.
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void) entry;
        ++entries;
    }
    CHECK(entries == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("file_license_store keeps a hard-linked license shared")
{
    // Writing a new file and renaming it into place would replace only the name
    // written through, leaving the other name on the old license.
    const auto dir = std::filesystem::temp_directory_path() / ("moonbase-cpp-hardlink-" + unique_suffix());
    std::filesystem::create_directories(dir);
    const auto path = dir / "license.mb";
    const auto alias = dir / "alias.mb";
    file_license_store(path).store_local_license(sample_license());
    std::filesystem::create_hard_link(path, alias);

    auto updated = sample_license();
    updated.id = "license-456";
    file_license_store(alias).store_local_license(updated);

    const auto loaded = file_license_store(path).load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-456");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
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

TEST_CASE("file_license_store::cancel_lock_waits ends a wait for a lock held elsewhere")
{
    const auto path = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-lock-cancel-" + unique_suffix() + ".json");

    // Another instance holds the lock, as it does for its whole online check.
    file_license_store sibling(path);
    file_license_store store(path);
    auto held = sibling.lock_for_update();

    std::atomic<bool> finished{false};
    std::atomic<bool> gave_up{false};
    std::thread waiting([&] {
        try {
            auto guard = store.lock_for_update();
        } catch (const storage_error&) {
            gave_up = true;
        }
        finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // waiting by now

    store.cancel_lock_waits();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(finished.load());
    CHECK(gave_up.load());
    held.reset(); // a waiter that ignored the cancel finishes now, rather than hang the run
    waiting.join();

    // Later calls don't wait for a lock that is taken either...
    held = sibling.lock_for_update();
    CHECK_THROWS_AS((void) store.lock_for_update(), storage_error);
    // ...but still take one that is free.
    held.reset();
    CHECK(store.lock_for_update() != nullptr);

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

TEST_CASE("file_license_store wraps a blocked directory creation in storage_error with the path")
{
    // Regression: create_directories used to run without an error_code, so a
    // failure escaped as a raw std::filesystem::filesystem_error that consumers
    // catching storage_error would miss. Place a regular file where store needs
    // a directory so the parent can never be created.
    const auto blocker = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-parent-is-file-" + unique_suffix());
    {
        std::ofstream file(blocker);
        file << "not a directory";
    }
    REQUIRE(std::filesystem::is_regular_file(blocker));

    // The license path is nested under the regular file, so create_directories
    // of its parent (<blocker>/nested) must fail.
    const auto path = blocker / "nested" / "license.mb";
    file_license_store store(path);

    bool threw_storage_error = false;
    std::string message;
    try {
        store.store_local_license(sample_license());
    } catch (const storage_error& ex) {
        threw_storage_error = true;
        message = ex.what();
    }
    CHECK(threw_storage_error);
    CHECK(message.find(blocker.string()) != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(blocker, ec);
}

TEST_CASE("file_license_store names a non-ASCII path in UTF-8 in its storage_error")
{
    // Regression: messages used path::string(), which on Windows converts to the
    // ANSI code page, and MSVC throws std::system_error for a letter with no
    // mapping there, so the storage_error never got built. char16_t is UTF-16 by
    // the standard, which keeps this portable without the C++20-deprecated u8path.
    auto name = std::filesystem::path(u"moonbase-cpp-Bj\u00f6rn-\u65e5\u672c-");
    name += unique_suffix();
    const auto blocker = std::filesystem::temp_directory_path() / name;
    {
        std::ofstream file(blocker);
        file << "not a directory";
    }
    REQUIRE(std::filesystem::is_regular_file(blocker));

    file_license_store store(blocker / "nested" / "license.mb");

    bool threw_storage_error = false;
    std::string message;
    try {
        store.store_local_license(sample_license());
    } catch (const storage_error& ex) {
        threw_storage_error = true;
        message = ex.what();
    }
    CHECK(threw_storage_error);
    CHECK(message.find("Bj\xc3\xb6rn-\xe6\x97\xa5\xe6\x9c\xac-") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(blocker, ec);
}

#ifndef _WIN32
TEST_CASE("file_license_store reports a storage_error with the path when the license file is unreadable")
{
    // Permission bits don't constrain root, so the chmod below wouldn't deny us.
    if (::geteuid() == 0)
        return;

    const auto path = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-unreadable-" + unique_suffix() + ".json");
    {
        file_license_store seed(path);
        seed.store_local_license(sample_license());
    }
    REQUIRE(::chmod(path.c_str(), 0) == 0); // strip all permissions

    file_license_store store(path);
    bool threw = false;
    std::string message;
    try {
        (void) store.load_local_license();
    } catch (const storage_error& ex) {
        threw = true;
        message = ex.what();
    }
    CHECK(threw);
    CHECK(message.find(path.string()) != std::string::npos);

    ::chmod(path.c_str(), 0600); // restore so cleanup can remove it
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("file_license_store reports a storage_error with the path when the directory is read-only")
{
    if (::geteuid() == 0)
        return;

    const auto dir = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-ro-dir-" + unique_suffix());
    std::filesystem::create_directories(dir);
    REQUIRE(::chmod(dir.c_str(), 0500) == 0); // r-x: cannot create files within

    const auto path = dir / "license.mb";
    file_license_store store(path);
    bool threw = false;
    std::string message;
    try {
        store.store_local_license(sample_license());
    } catch (const storage_error& ex) {
        threw = true;
        message = ex.what();
    }
    CHECK(threw);
    CHECK(message.find(path.string()) != std::string::npos);

    ::chmod(dir.c_str(), 0700); // restore so cleanup can recurse
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("file_license_store keeps a license restricted to its owner that way")
{
    const auto dir = std::filesystem::temp_directory_path() / ("moonbase-cpp-mode-" + unique_suffix());
    std::filesystem::create_directories(dir);
    const auto path = dir / "license.mb";

    // A umask that leaves new files readable by everyone, as most do.
    const auto previous_umask = ::umask(022);
    file_license_store store(path);
    store.store_local_license(sample_license());
    REQUIRE(::chmod(path.c_str(), 0600) == 0);

    store.store_local_license(sample_license());
    ::umask(previous_umask);

    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("file_license_store writes through a symlinked license path")
{
    // The link must keep pointing at the license, not be replaced by a copy
    // while what it pointed at keeps the old one.
    const auto dir = std::filesystem::temp_directory_path() / ("moonbase-cpp-symlink-" + unique_suffix());
    std::filesystem::create_directories(dir);
    const auto target = dir / "target.mb";
    const auto link = dir / "license.mb";
    file_license_store(target).store_local_license(sample_license());
    std::filesystem::create_symlink("target.mb", link); // relative, as links often are

    auto updated = sample_license();
    updated.id = "license-456";
    file_license_store(link).store_local_license(updated);

    CHECK(std::filesystem::is_symlink(link));
    const auto loaded = file_license_store(target).load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-456");

    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void) entry;
        ++entries;
    }
    CHECK(entries == 2); // the link and its target, no leftovers

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("file_license_store rewrites the license in place in a folder that takes no new files")
{
    if (::geteuid() == 0)
        return;

    const auto dir = std::filesystem::temp_directory_path() /
        ("moonbase-cpp-fixed-dir-" + unique_suffix());
    std::filesystem::create_directories(dir);
    const auto path = dir / "license.mb";
    {
        file_license_store seed(path);
        seed.store_local_license(sample_license());
    }
    // The license stays writable, but nothing new can be created beside it, so
    // there is nowhere to write a replacement first.
    REQUIRE(::chmod(dir.c_str(), 0500) == 0);

    file_license_store store(path);
    auto updated = sample_license();
    updated.id = "license-456";
    CHECK_NOTHROW(store.store_local_license(updated));
    const auto loaded = store.load_local_license();
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "license-456");

    ::chmod(dir.c_str(), 0700);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif // _WIN32
