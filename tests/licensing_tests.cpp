#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "moonbase/fingerprint.hpp"
#include "moonbase/licensing.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

struct facade_fixture {
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    std::shared_ptr<static_fingerprint_provider> fingerprints =
        std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
    std::shared_ptr<moonbase::tests::recording_transport> transport =
        std::make_shared<moonbase::tests::recording_transport>();
    licensing instance;

    explicit facade_fixture(licensing_options options = {})
        : instance(prepare(std::move(options)), nullptr, fingerprints, transport)
    {
    }

    licensing_options prepare(licensing_options options)
    {
        if (options.endpoint.empty()) {
            options.endpoint = "https://demo.moonbase.sh";
        }
        if (options.product_id.empty()) {
            options.product_id = "demo-app";
        }
        if (options.public_key.empty()) {
            options.public_key = key.public_pem;
        }
        if (!options.account_id) {
            options.account_id = "tenant-1";
        }
        return options;
    }

    std::string make_token(nlohmann::json claims)
    {
        return moonbase::tests::make_token(key.key.get(), std::move(claims));
    }
};

} // namespace

TEST_CASE("validate_token_online skips API call within the min interval")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["validated"] = moonbase::tests::now_seconds() - 30; // well under default 5 min
    const auto token = fixture.make_token(claims);

    const auto result = fixture.instance.validate_token_online(token);

    CHECK(result.id == "license-123");
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("validate_token_online calls API after the min interval and returns refreshed license")
{
    facade_fixture fixture;
    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60); // 10 min > 5 min default
    const auto stale_token = fixture.make_token(stale);

    auto refreshed = moonbase::tests::default_claims();
    refreshed["validated"] = moonbase::tests::now_seconds();
    const auto refreshed_token = fixture.make_token(refreshed);
    fixture.transport->responses.push_back(http_response{200, {}, refreshed_token});

    const auto result = fixture.instance.validate_token_online(stale_token);

    CHECK(result.token == refreshed_token);
    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(fixture.transport->requests.front().url.find("/api/client/licenses/demo-app/validate") != std::string::npos);
}

TEST_CASE("validate_token_online tolerates transport failures within the grace period")
{
    facade_fixture fixture;
    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale);
    // No queued responses → recording_transport throws std::runtime_error.

    const auto result = fixture.instance.validate_token_online(stale_token);

    CHECK(result.id == "license-123");
    CHECK(result.token == stale_token);
    CHECK(fixture.transport->requests.size() == 1);
}

TEST_CASE("validate_token_online fails when transport fails and grace has elapsed")
{
    licensing_options options;
    options.online_validation_grace_period = std::chrono::seconds(60);
    options.online_validation_min_interval = std::chrono::seconds(10);
    facade_fixture fixture(options);

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (5 * 60); // 5 min > 60s grace
    const auto stale_token = fixture.make_token(stale);

    CHECK_THROWS_AS(
        (void)fixture.instance.validate_token_online(stale_token),
        std::exception);
}

TEST_CASE("validate_token_online propagates definitive license errors regardless of grace")
{
    licensing_options options;
    options.online_validation_grace_period = std::chrono::hours(24 * 365);
    facade_fixture fixture(options);

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale);

    SUBCASE("invalid")
    {
        fixture.transport->responses.push_back(
            http_response{400, {}, R"({"title":"Invalid","detail":"bad"})"});
        CHECK_THROWS_AS(
            (void)fixture.instance.validate_token_online(stale_token),
            license_invalid_error);
    }

    SUBCASE("expired")
    {
        fixture.transport->responses.push_back(
            http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"expired"})"});
        CHECK_THROWS_AS(
            (void)fixture.instance.validate_token_online(stale_token),
            license_expired_error);
    }
}

TEST_CASE("validate_token_online never contacts the API for offline-activated tokens")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["method"] = "Offline";
    claims["validated"] = moonbase::tests::now_seconds() - (24 * 60 * 60 * 365); // ancient
    const auto token = fixture.make_token(claims);

    const auto result = fixture.instance.validate_token_online(token);

    CHECK(result.method == activation_method::offline);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("validate_token_online runs local validation before any HTTP call")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims("other-device");
    claims["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto token = fixture.make_token(claims);

    CHECK_THROWS_AS(
        (void)fixture.instance.validate_token_online(token),
        license_invalid_error);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("validate_token_online refuses to let min_interval extend past grace_period")
{
    licensing_options options;
    options.online_validation_grace_period = std::chrono::seconds(60);
    options.online_validation_min_interval = std::chrono::hours(1); // longer than grace
    facade_fixture fixture(options);

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - 120; // past grace, within throttle
    const auto stale_token = fixture.make_token(stale);
    // No queued response → recording_transport throws std::runtime_error.
    // With the throttle bypassing grace, the call would silently return the
    // stale local token. With the fix, it must attempt the API and propagate.
    CHECK_THROWS_AS(
        (void)fixture.instance.validate_token_online(stale_token),
        std::exception);
    CHECK(fixture.transport->requests.size() == 1);
}

TEST_CASE("validate_token_online honors a custom min interval")
{
    licensing_options options;
    options.online_validation_min_interval = std::chrono::seconds(1);
    facade_fixture fixture(options);

    auto claims = moonbase::tests::default_claims();
    claims["validated"] = moonbase::tests::now_seconds() - 30; // > 1s custom interval
    const auto token = fixture.make_token(claims);

    auto refreshed = moonbase::tests::default_claims();
    refreshed["validated"] = moonbase::tests::now_seconds();
    fixture.transport->responses.push_back(
        http_response{200, {}, fixture.make_token(refreshed)});

    (void)fixture.instance.validate_token_online(token);

    CHECK(fixture.transport->requests.size() == 1);
}

TEST_CASE("revoke_activation refuses offline tokens without contacting the API")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["method"] = "Offline";
    const auto token = fixture.make_token(claims);

    CHECK_THROWS_AS(fixture.instance.revoke_activation(token),
                    operation_not_supported_error);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("revoke_activation refuses trial tokens without contacting the API")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["trial"] = true;
    const auto token = fixture.make_token(claims);

    CHECK_THROWS_AS(fixture.instance.revoke_activation(token),
                    operation_not_supported_error);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("revoke_activation calls the API and clears the matching local license")
{
    facade_fixture fixture;
    const auto token = fixture.make_token(moonbase::tests::default_claims());
    const auto stored = fixture.instance.validator().validate_token(token);
    fixture.instance.store().store_local_license(stored);

    fixture.transport->responses.push_back(http_response{200, {}, ""});

    fixture.instance.revoke_activation(token);

    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(fixture.transport->requests.front().url.find(
              "/api/client/licenses/demo-app/revoke") != std::string::npos);
    CHECK_FALSE(fixture.instance.store().load_local_license().has_value());
}

TEST_CASE("revoke_activation leaves an unrelated stored license alone")
{
    facade_fixture fixture;

    auto stored_claims = moonbase::tests::default_claims();
    stored_claims["id"] = "activation-A";
    const auto stored_token = fixture.make_token(stored_claims);
    const auto stored_license =
        fixture.instance.validator().validate_token(stored_token);
    fixture.instance.store().store_local_license(stored_license);

    auto revoke_claims = moonbase::tests::default_claims();
    revoke_claims["id"] = "activation-B";
    const auto revoke_token = fixture.make_token(revoke_claims);

    fixture.transport->responses.push_back(http_response{200, {}, ""});

    fixture.instance.revoke_activation(revoke_token);

    REQUIRE(fixture.transport->requests.size() == 1);
    const auto remaining = fixture.instance.store().load_local_license();
    REQUIRE(remaining.has_value());
    CHECK(remaining->activation_id == "activation-A");
}

TEST_CASE("revoke_activation still POSTs when the local token has expired")
{
    facade_fixture fixture;
    auto claims = moonbase::tests::default_claims();
    claims["exp"] = moonbase::tests::now_seconds() - 60; // already past
    const auto token = fixture.make_token(claims);

    fixture.transport->responses.push_back(http_response{200, {}, ""});

    // Must not throw license_expired_error; the seat is still allocated
    // server-side and the request should reach the API.
    fixture.instance.revoke_activation(token);

    REQUIRE(fixture.transport->requests.size() == 1);
    CHECK(fixture.transport->requests.front().url.find(
              "/api/client/licenses/demo-app/revoke") != std::string::npos);
}

namespace {

class throwing_license_store : public license_store {
public:
    std::optional<license> load_local_license() override
    {
        throw storage_error("load failed");
    }

    void store_local_license(const license&) override {}

    void delete_local_license() override
    {
        throw storage_error("delete failed");
    }
};

} // namespace

TEST_CASE("revoke_activation succeeds even if local store cleanup fails")
{
    auto fingerprints =
        std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
    auto transport = std::make_shared<moonbase::tests::recording_transport>();
    auto store = std::make_shared<throwing_license_store>();

    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.public_key = key.public_pem;
    options.account_id = "tenant-1";

    licensing instance(std::move(options), store, fingerprints, transport);

    const auto token = moonbase::tests::make_token(
        key.key.get(), moonbase::tests::default_claims());
    transport->responses.push_back(http_response{200, {}, ""});

    // The server-side seat is freed the moment the API returns 200 — a local
    // storage failure must not turn that into a thrown error that callers
    // would interpret as "retry against a token the server no longer knows".
    instance.revoke_activation(token);

    REQUIRE(transport->requests.size() == 1);
}

namespace {

// Thread-safe counting transport for the concurrent-dedup test. recording_transport's
// vector/deque mutations aren't safe across threads, and we want a hard assertion
// on call count rather than relying on the "no queued response → throw" failure mode.
class counting_transport : public http_transport {
public:
    std::atomic<int> count{0};
    std::string response_body;

    http_response send(const http_request&) override
    {
        count.fetch_add(1, std::memory_order_relaxed);
        return http_response{200, {}, response_body};
    }
};

// Wraps another store to count lock_for_update() calls. Used to verify the
// SDK acquires the cross-process lock around the validate-online critical section.
class tracking_store : public license_store {
public:
    std::atomic<int> lock_count{0};

    std::optional<license> load_local_license() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    void store_local_license(const license& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = value;
    }

    void delete_local_license() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.reset();
    }

    std::unique_ptr<store_lock_guard> lock_for_update() override
    {
        lock_count.fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<store_lock_guard>();
    }

private:
    std::mutex mutex_;
    std::optional<license> value_;
};

} // namespace

TEST_CASE("validate_token_online deduplicates concurrent in-process callers")
{
    // Set up a fresh facade with a thread-safe counting transport instead of
    // recording_transport (which is not safe under concurrent send()).
    auto fingerprints =
        std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
    auto transport = std::make_shared<counting_transport>();

    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.public_key = key.public_pem;
    options.account_id = "tenant-1";

    licensing instance(std::move(options), nullptr, fingerprints, transport);

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60); // > 5 min min_interval
    const auto stale_token = moonbase::tests::make_token(key.key.get(), stale);

    // The refreshed token the (single) network call will return.
    auto refreshed = moonbase::tests::default_claims();
    refreshed["validated"] = moonbase::tests::now_seconds();
    transport->response_body =
        moonbase::tests::make_token(key.key.get(), refreshed);

    constexpr int thread_count = 16;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> success{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                const auto result = instance.validate_token_online(stale_token);
                if (result.id == "license-123") {
                    success.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                // Counted as failure via success not incrementing.
            }
        });
    }

    while (ready.load() < thread_count) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& t : threads) t.join();

    CHECK(success.load() == thread_count);
    CHECK(transport->count.load() == 1);
}

TEST_CASE("validate_token_online acquires the store update lock once per online check")
{
    auto fingerprints =
        std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
    auto transport = std::make_shared<moonbase::tests::recording_transport>();
    auto store = std::make_shared<tracking_store>();

    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.public_key = key.public_pem;
    options.account_id = "tenant-1";

    licensing instance(std::move(options), store, fingerprints, transport);

    auto fresh_claims = moonbase::tests::default_claims();
    fresh_claims["validated"] = moonbase::tests::now_seconds() - 30; // within throttle window
    const auto fresh_token =
        moonbase::tests::make_token(key.key.get(), fresh_claims);

    // Within the throttle window the SDK must still take the lock — that's
    // exactly the window where two racing processes need to see each other's
    // freshly-persisted validated_at without hitting the network.
    (void)instance.validate_token_online(fresh_token);
    CHECK(store->lock_count.load() == 1);
    CHECK(transport->requests.empty());

    // Offline-activated tokens short-circuit before the lock — no extra
    // contention for processes coordinating on the file.
    auto offline_claims = moonbase::tests::default_claims();
    offline_claims["method"] = "Offline";
    const auto offline_token =
        moonbase::tests::make_token(key.key.get(), offline_claims);
    (void)instance.validate_token_online(offline_token);
    CHECK(store->lock_count.load() == 1);
}

TEST_CASE("validate_token_online persists the refreshed license itself")
{
    facade_fixture fixture;

    auto stale = moonbase::tests::default_claims();
    stale["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale);

    auto refreshed_claims = moonbase::tests::default_claims();
    refreshed_claims["validated"] = moonbase::tests::now_seconds();
    const auto refreshed_token = fixture.make_token(refreshed_claims);
    fixture.transport->responses.push_back(http_response{200, {}, refreshed_token});

    const auto returned = fixture.instance.validate_token_online(stale_token);
    CHECK(returned.token == refreshed_token);

    // The SDK now writes the refresh back so that sibling instances/processes
    // observe the new validated_at on their next call.
    auto stored = fixture.instance.store().load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == refreshed_token);
}

TEST_CASE("validate_token_online prefers the freshest stored license when reloading under lock")
{
    facade_fixture fixture;

    // The caller passes a stale token (e.g. cached in a sibling plugin instance's
    // memory), but another process has already persisted a fresher one. The SDK
    // re-reads the store under the lock and uses the fresher timestamp for the
    // throttle check, so no network call is needed.
    auto stale_claims = moonbase::tests::default_claims();
    stale_claims["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale_claims);

    auto fresh_claims = moonbase::tests::default_claims();
    fresh_claims["validated"] = moonbase::tests::now_seconds() - 10; // well under min_interval
    const auto fresh_token = fixture.make_token(fresh_claims);
    auto fresh_license = fixture.instance.validator().validate_token(fresh_token);
    fixture.instance.store().store_local_license(fresh_license);

    const auto result = fixture.instance.validate_token_online(stale_token);

    CHECK(fixture.transport->requests.empty());
    CHECK(result.token == fresh_token);
}

TEST_CASE("validate_token_online skips persist when should_persist returns false")
{
    // Models the bridge's "user pressed Deactivate (or activated a different
    // license) while a background revalidation was in flight" race. The
    // refreshed token must still be returned to the caller (it's a valid
    // license — the caller just chooses to discard it), but it must NOT be
    // written to the store, or else the next launch or sibling instance
    // would resurrect the activation the user just cleared.
    facade_fixture fixture;

    auto stale_claims = moonbase::tests::default_claims();
    stale_claims["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale_claims);

    auto refreshed_claims = moonbase::tests::default_claims();
    refreshed_claims["validated"] = moonbase::tests::now_seconds();
    const auto refreshed_token = fixture.make_token(refreshed_claims);
    fixture.transport->responses.push_back(http_response{200, {}, refreshed_token});

    int predicate_calls = 0;
    const auto returned = fixture.instance.validate_token_online(
        stale_token,
        [&] { ++predicate_calls; return false; });

    CHECK(predicate_calls == 1);
    CHECK(returned.token == refreshed_token);
    CHECK_FALSE(fixture.instance.store().load_local_license().has_value());
}

TEST_CASE("validate_token_online persists when should_persist returns true")
{
    facade_fixture fixture;

    auto stale_claims = moonbase::tests::default_claims();
    stale_claims["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = fixture.make_token(stale_claims);

    auto refreshed_claims = moonbase::tests::default_claims();
    refreshed_claims["validated"] = moonbase::tests::now_seconds();
    const auto refreshed_token = fixture.make_token(refreshed_claims);
    fixture.transport->responses.push_back(http_response{200, {}, refreshed_token});

    const auto returned = fixture.instance.validate_token_online(
        stale_token, [] { return true; });

    CHECK(returned.token == refreshed_token);
    auto stored = fixture.instance.store().load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == refreshed_token);
}

TEST_CASE("validate_token_online does not invoke should_persist on the throttle fast path")
{
    facade_fixture fixture;

    auto fresh_claims = moonbase::tests::default_claims();
    fresh_claims["validated"] = moonbase::tests::now_seconds() - 30; // within min_interval
    const auto fresh_token = fixture.make_token(fresh_claims);

    int predicate_calls = 0;
    (void)fixture.instance.validate_token_online(
        fresh_token,
        [&] { ++predicate_calls; return true; });

    CHECK(predicate_calls == 0);
    CHECK(fixture.transport->requests.empty());
}

TEST_CASE("revoke_activation acquires the store update lock around its cleanup")
{
    // Without the lock, an in-flight validate_token_online in a sibling
    // instance could persist between revoke_activation's load and delete,
    // resurrecting the license the user just revoked. The lock makes the
    // load+delete atomic with respect to any concurrent persist.
    auto fingerprints =
        std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
    auto transport = std::make_shared<moonbase::tests::recording_transport>();
    auto store = std::make_shared<tracking_store>();

    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.public_key = key.public_pem;
    options.account_id = "tenant-1";

    licensing instance(std::move(options), store, fingerprints, transport);

    const auto token = moonbase::tests::make_token(
        key.key.get(), moonbase::tests::default_claims());
    transport->responses.push_back(http_response{200, {}, ""});

    const auto baseline = store->lock_count.load();
    instance.revoke_activation(token);

    CHECK(store->lock_count.load() > baseline);
}
