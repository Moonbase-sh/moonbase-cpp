#include <doctest/doctest.h>

// Cross-process deduplication integration test. POSIX-only (uses fork()).
// On Windows this test compiles to nothing — coverage on Windows comes from
// the in-process and file-lock unit tests, plus the Win32-specific file_lock
// path in detail/file_lock.hpp.
#if !defined(_WIN32)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "moonbase/fingerprint.hpp"
#include "moonbase/http.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/store.hpp"

#include "test_helpers.hpp"

using namespace moonbase;

namespace {

// Transport that atomically increments an on-disk integer counter under
// flock() on every send(). The counter file is what the parent test inspects
// post-fork to assert "exactly one process actually hit the network."
class counting_file_transport : public http_transport {
public:
    std::filesystem::path counter_path;
    std::string response_body;

    http_response send(const http_request&) override
    {
        const int fd = ::open(counter_path.c_str(), O_RDWR | O_CREAT, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::flock(fd, LOCK_EX) == 0);

        std::int64_t value = 0;
        char buf[32] = {0};
        const auto bytes = ::pread(fd, buf, sizeof(buf) - 1, 0);
        if (bytes > 0) {
            value = std::strtoll(buf, nullptr, 10);
        }
        ++value;

        const auto out = std::to_string(value);
        ::ftruncate(fd, 0);
        ::pwrite(fd, out.data(), out.size(), 0);

        ::flock(fd, LOCK_UN);
        ::close(fd);

        return http_response{200, {}, response_body};
    }
};

std::int64_t read_counter(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) return 0;
    std::int64_t value = 0;
    in >> value;
    return value;
}

std::filesystem::path unique_temp_path(const std::string& tag)
{
    return std::filesystem::temp_directory_path() /
        ("moonbase-cpp-" + tag + "-"
         + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
         + "-" + std::to_string(::getpid()));
}

} // namespace

TEST_CASE("validate_token_online deduplicates across forked processes")
{
    // Set up the world in the parent so all children inherit a fully-baked
    // signing key + tokens. OpenSSL state lives in heap memory and survives
    // fork's copy-on-write snapshot cleanly here because no OpenSSL global
    // (e.g. RNG) is touched at validate time on the read path.
    moonbase::tests::generated_key key = moonbase::tests::generate_key();

    auto stale_claims = moonbase::tests::default_claims();
    stale_claims["validated"] = moonbase::tests::now_seconds() - (10 * 60);
    const auto stale_token = moonbase::tests::make_token(key.key.get(), stale_claims);

    auto fresh_claims = moonbase::tests::default_claims();
    fresh_claims["validated"] = moonbase::tests::now_seconds();
    const auto refreshed_token =
        moonbase::tests::make_token(key.key.get(), fresh_claims);

    const auto store_path = unique_temp_path("xproc-store") += ".json";
    const auto counter_path = unique_temp_path("xproc-counter") += ".txt";

    licensing_options opts;
    opts.endpoint = "https://demo.moonbase.sh";
    opts.product_id = "demo-app";
    opts.public_key = key.public_pem;
    opts.account_id = "tenant-1";

    // Seed the store with a stale-but-locally-valid license so each child
    // exercises the "stale → must check online" path on entry. The
    // cross-process file lock + SDK persist-on-success then collapses N
    // children into a single network call.
    {
        license_validator seeder(
            opts,
            std::make_shared<static_fingerprint_provider>("Test Device", "device-id"));
        file_license_store seed(store_path);
        seed.store_local_license(seeder.validate_token(stale_token));
    }

    constexpr int child_count = 8;
    std::vector<pid_t> children;
    children.reserve(child_count);

    for (int i = 0; i < child_count; ++i) {
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            // Child: run one validate_token_online call against the shared
            // file store + shared counter, then _exit() to bypass doctest's
            // atexit teardown (which would otherwise double-report).
            try {
                auto fingerprint =
                    std::make_shared<static_fingerprint_provider>("Test Device", "device-id");
                auto transport = std::make_shared<counting_file_transport>();
                transport->counter_path = counter_path;
                transport->response_body = refreshed_token;
                auto store = std::make_shared<file_license_store>(store_path);

                licensing instance(opts, store, fingerprint, transport);
                (void)instance.validate_token_online(stale_token);
                ::_exit(0);
            } catch (...) {
                ::_exit(1);
            }
        }
        children.push_back(pid);
    }

    int failed = 0;
    for (const auto pid : children) {
        int status = 0;
        const auto rc = ::waitpid(pid, &status, 0);
        REQUIRE(rc == pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            ++failed;
        }
    }
    CHECK(failed == 0);

    const auto counter = read_counter(counter_path);
    CHECK(counter == 1);

    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    std::filesystem::remove(counter_path, ec);
}

#endif // !_WIN32
