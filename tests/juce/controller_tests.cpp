// Unit tests for the JUCE module's ActivationController state machine.
//
// The controller talks to moonbase::licensing directly and marshals async
// results back onto the message thread via MessageManager::callAsync, so these
// tests inject a licensing built from fake store / transport / fingerprint (the
// same doubles the SDK tests use) and pump the JUCE message loop to completion.
//
// The module header is included first so MOONBASE_CRYPTO_NATIVE is defined for
// the whole TU, matching how the module compiles the SDK (verify with the OS
// backend). Tokens are still *signed* with OpenSSL in test_helpers.hpp - a
// deliberate cross-backend round-trip (OpenSSL sign -> native verify).

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <moonbase_licensing/moonbase_licensing.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "test_helpers.hpp"

using namespace moonbase::juce_integration;
using moonbase::tests::default_claims;
using moonbase::tests::now_seconds;
using moonbase::tests::recording_transport;
using Screen = ActivationController::Screen;

namespace {

// Run the JUCE message loop in short slices until cond() holds or we time out.
// The controller's background threads post their results back with callAsync,
// which only runs while the loop is pumped.
bool pumpUntil(const std::function<bool()>& cond, int timeoutMs = 5000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto start = juce::Time::getMillisecondCounter();
    while (! cond())
    {
        mm->runDispatchLoopUntil(15);
        if ((int) (juce::Time::getMillisecondCounter() - start) > timeoutMs)
            break;
    }
    return cond();
}

// Keep the message loop running for a while, e.g. to show that something does
// NOT happen.
void pumpFor(int ms)
{
    pumpUntil([] { return false; }, ms);
}

bool settled(const ActivationController& c)
{
    return c.screen() != Screen::Loading;
}

struct controller_fixture
{
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    std::shared_ptr<moonbase::static_device_id_resolver> fingerprint =
        std::make_shared<moonbase::static_device_id_resolver>("Studio Mac", "device-id");
    std::shared_ptr<recording_transport> transport = std::make_shared<recording_transport>();
    std::shared_ptr<moonbase::file_license_store> store;
    juce::File licenseFile;
    ActivationConfig config;

    controller_fixture()
    {
        licenseFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("moonbase-juce-tests")
                          .getChildFile(juce::Uuid().toString() + ".mb");
        licenseFile.getParentDirectory().createDirectory();
        licenseFile.deleteFile();
        store = std::make_shared<moonbase::file_license_store>(compat::toFilesystemPath(licenseFile));

        config.endpoint = "https://demo.moonbase.sh";
        config.productId = "demo-app";
        config.publicKey = key.public_pem;
        config.accountId = "tenant-1";
        config.productName = "Solstice";
        config.manufacturerName = "Helio Audio";
        config.licenseFile = licenseFile; // keep sibling state (e.g. the .state.json file)
                                          // in the temp dir, not real app data
    }

    ~controller_fixture()
    {
        licenseFile.deleteFile();
        licenseFile.getParentDirectory().deleteRecursively();
    }

    std::shared_ptr<moonbase::licensing> makeLicensing()
    {
        return std::make_shared<moonbase::licensing>(
            config.toLicensingOptions(), store, fingerprint, transport);
    }

    std::string token(nlohmann::json claims)
    {
        return moonbase::tests::make_token(key.key.get(), std::move(claims));
    }

    // Persist a token into the store exactly as the controller would find it at
    // launch (validated allowing expiry so even a dead token can be seeded).
    void seedStored(const std::string& tok)
    {
        auto licensing = makeLicensing();
        auto lic = licensing->validate_token_local_allow_expired(tok);
        auto guard = store->lock_for_update();
        store->store_local_license(lic);
    }

    // Persist a token without validating it - used to plant a token the loader
    // will reject (e.g. issued for another device), which validate-then-store
    // can't do because validation throws first.
    void seedRawToken(const std::string& tok)
    {
        moonbase::license lic;
        lic.token = tok;
        lic.id = "license-123";
        lic.activation_id = "activation-123";
        lic.licensed_product.id = config.productId.toStdString();
        lic.licensed_product.name = "Demo Product";
        lic.issued_to.id = "user-123";
        lic.issued_to.name = "Jane Developer";
        lic.issued_to.email = "jane@example.com";
        auto guard = store->lock_for_update();
        store->store_local_license(lic);
    }
};

// Holds every request at a gate until the test releases it, then answers from
// the queue (or fails, like a dropped connection, when it runs dry). For
// arranging what lands while a request is still on its way.
struct gated_transport : moonbase::http_transport
{
    juce::WaitableEvent gate{ true }; // manual reset: once released, stays open
    std::atomic<bool> entered{ false };
    std::atomic<int> requests{ 0 };
    std::mutex mutex;
    std::deque<moonbase::http_response> responses;

    moonbase::http_response send(const moonbase::http_request&) override
    {
        ++requests;
        entered = true;
        gate.wait();
        std::lock_guard<std::mutex> lock(mutex);
        if (responses.empty())
            throw moonbase::api_error(0, "no response queued");
        auto response = responses.front();
        responses.pop_front();
        return response;
    }

    void release() { gate.signal(); }
};

} // namespace

//==============================================================================
// start(): routing from a stored license
//==============================================================================
TEST_CASE("start() with no stored license routes to Welcome")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Welcome);
    CHECK_FALSE(controller.license().has_value());
    CHECK(controller.lockReason() == LockReason::None); // never licensed: nothing to explain
}

TEST_CASE("start() with a valid online license routes to Details without calling the API")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims())); // online, validated 30s ago, exp in future

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->method == moonbase::activation_method::online);
    // Validated within the min interval -> the throttle skips the network.
    CHECK(fx.transport->requests.empty());
}

TEST_CASE("start() with a valid trial license routes to Trial")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Trial);
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->trial);
    CHECK_FALSE(controller.trialIsEndless());
    CHECK(controller.trialDaysRemaining() == 1); // exp is an hour out, rounded up to a day
}

TEST_CASE("an endless trial routes to Trial with no countdown and stays unlocked")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    claims.erase("exp"); // the product's trials never expire
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Trial);
    REQUIRE(controller.license().has_value());
    CHECK_FALSE(controller.license()->expires_at.has_value());
    CHECK(controller.trialIsEndless());
    CHECK(controller.trialDaysRemaining() == 0);

    // The license watch has nothing to count down to, so a tick leaves it unlocked
    pumpUntil([] { return false; }, 2500);
    CHECK(controller.screen() == Screen::Trial);
    CHECK(controller.licensedFlag().load());
    CHECK(controller.lockReason() == LockReason::None);
}

TEST_CASE("an endless trial the vendor ends shows Expired")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    claims.erase("exp");
    claims["validated"] = now_seconds() - 3600; // past the throttle -> start() re-checks online
    fx.seedStored(fx.token(claims));

    fx.transport->responses.push_back(
        moonbase::http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"trial ended"})"});

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Expired);
    CHECK_FALSE(controller.license().has_value());
    REQUIRE(controller.expiredTrial().has_value());
    CHECK_FALSE(controller.expiredTrial()->expires_at.has_value());
    CHECK(controller.lockReason() == LockReason::Expired);
}

TEST_CASE("an expired trial shows the Expired screen and keeps the plugin locked")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    claims["exp"] = now_seconds() - 10; // trial ended
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Expired);
    // The plugin must stay locked: license() is empty so DSP gating bypasses...
    CHECK_FALSE(controller.license().has_value());
    // ...but the ended trial is available for the screen to display.
    REQUIRE(controller.expiredTrial().has_value());
    CHECK(controller.expiredTrial()->trial);
    CHECK(controller.lockReason() == LockReason::Expired);
    CHECK(fx.transport->requests.empty()); // an expired trial never hits the API
}

TEST_CASE("a trial that is valid locally but re-validates as expired shows Expired")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    claims["validated"] = now_seconds() - 3600; // past the throttle -> start() re-checks online
    fx.seedStored(fx.token(claims));             // exp is still in the future -> valid locally

    // The server reports the trial has ended.
    fx.transport->responses.push_back(
        moonbase::http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"trial ended"})"});

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Expired);
    CHECK_FALSE(controller.license().has_value());
    REQUIRE(controller.expiredTrial().has_value());
    CHECK(fx.transport->requests.size() == 1); // it did re-check online
}

TEST_CASE("refreshLicense that finds the trial expired shows Expired and locks")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    fx.seedStored(fx.token(claims)); // valid trial, validated recently -> start() stays on Trial

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));
    REQUIRE(controller.license().has_value());

    // A forced re-check; the server says the trial has ended.
    fx.transport->responses.push_back(
        moonbase::http_response{400, {}, R"({"errorType":"LicenseExpired","detail":"trial ended"})"});

    bool done = false;
    controller.refreshLicense(true, [&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));

    CHECK(controller.screen() == Screen::Expired);
    CHECK_FALSE(controller.license().has_value());
    REQUIRE(controller.expiredTrial().has_value());
}

TEST_CASE("showDetails on a trial stays on the trial view, not Details")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));

    controller.showDetails();
    CHECK(controller.screen() == Screen::Trial);
}

//==============================================================================
// App update flow (newer released version than the running app)
//==============================================================================
TEST_CASE("a license whose released version outranks the app routes to UpdateAvailable")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.2"; // older than default_claims p:rel (1.2.3)
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::UpdateAvailable);
    CHECK(controller.updateAvailable());
    CHECK(controller.updateInfo().newVersion == "1.2.3");
    CHECK(controller.updateInfo().currentVersion == "1.2.2");
    REQUIRE(controller.license().has_value()); // still licensed: gating stays on

    // "Skip this update" records the skip and routes back to Details.
    controller.dismissUpdate();
    CHECK(controller.screen() == Screen::Details);
    controller.showDetails();
    CHECK(controller.screen() == Screen::Details);
}

TEST_CASE("an up-to-date app routes straight to Details")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.3"; // equal to the released version
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details);
    CHECK_FALSE(controller.updateAvailable());
}

TEST_CASE("enableUpdatePrompt=false never shows the update screen")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.0.0"; // far behind, but updates are disabled
    fx.config.enableUpdatePrompt = false;
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details);
    CHECK_FALSE(controller.updateAvailable());
}

TEST_CASE("setPreviewUpdate forces the update screen with synthetic state")
{
    controller_fixture fx;
    fx.config.applicationVersion = "2.3.1";
    ActivationController controller(fx.config, fx.makeLicensing());

    auto claims = default_claims();
    claims["p:rel"] = "2.4.0";
    auto lic = fx.makeLicensing()->validate_token_local_allow_expired(fx.token(claims));

    controller.setPreviewUpdate(ActivationController::UpdateInfo::Phase::Ready, lic, "What's new");
    CHECK(controller.screen() == Screen::UpdateAvailable);
    CHECK(controller.updateInfo().newVersion == "2.4.0");
    CHECK(controller.updateInfo().currentVersion == "2.3.1");
    CHECK(controller.updateInfo().releaseNotes == "What's new");
}

TEST_CASE("dismissing the update is remembered across restarts")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.2"; // older than p:rel (1.2.3)
    fx.seedStored(fx.token(default_claims()));

    {
        ActivationController controller(fx.config, fx.makeLicensing());
        controller.start();
        REQUIRE(pumpUntil([&] { return settled(controller); }));
        REQUIRE(controller.screen() == Screen::UpdateAvailable);
        controller.dismissUpdate();
        CHECK(controller.screen() == Screen::Details);
    }

    // A fresh controller (same config + license file) must not prompt again: the
    // dismissal was persisted next to the license.
    ActivationController restarted(fx.config, fx.makeLicensing());
    restarted.start();
    REQUIRE(pumpUntil([&] { return settled(restarted); }));
    CHECK(restarted.screen() == Screen::Details);
    CHECK(restarted.updateAvailable()); // an update still exists...
}

TEST_CASE("a newer release re-prompts after an earlier dismissal")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.2";

    fx.seedStored(fx.token(default_claims())); // p:rel 1.2.3
    {
        ActivationController controller(fx.config, fx.makeLicensing());
        controller.start();
        REQUIRE(pumpUntil([&] { return settled(controller); }));
        REQUIRE(controller.screen() == Screen::UpdateAvailable);
        controller.dismissUpdate(); // remembers 1.2.3
    }

    // The product now ships an even newer release.
    auto newer = default_claims();
    newer["p:rel"] = "1.3.0";
    fx.seedStored(fx.token(newer));

    ActivationController restarted(fx.config, fx.makeLicensing());
    restarted.start();
    REQUIRE(pumpUntil([&] { return settled(restarted); }));
    CHECK(restarted.screen() == Screen::UpdateAvailable); // newer than the dismissed one
    CHECK(restarted.updateInfo().newVersion == "1.3.0");
}

TEST_CASE("showUpdate re-opens the update screen after a dismissal")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.2"; // older than p:rel (1.2.3)
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));
    REQUIRE(controller.screen() == Screen::UpdateAvailable);

    controller.dismissUpdate();
    REQUIRE(controller.screen() == Screen::Details);
    CHECK(controller.updateAvailable()); // still available, just dismissed

    controller.showUpdate(); // e.g. the "Update available" badge in the license view
    CHECK(controller.screen() == Screen::UpdateAvailable);
}

TEST_CASE("showUpdate is a no-op when the app is up to date")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.3"; // equal to p:rel -> no update
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    controller.showUpdate();
    CHECK(controller.screen() == Screen::Details); // nothing to show
}

TEST_CASE("autoPresentUpdate=false keeps startup on the license view")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.2"; // older than p:rel (1.2.3): update available
    fx.config.autoPresentUpdate = false;
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details); // no automatic pop on open
    CHECK(controller.updateAvailable());            // ...even though an update exists

    controller.showUpdate(/*fromLicenseView*/ true); // the badge still opens it
    CHECK(controller.screen() == Screen::UpdateAvailable);
}

TEST_CASE("the update screen tracks whether it was opened from the license view")
{
    controller_fixture fx;
    fx.config.applicationVersion = "1.2.2"; // older than p:rel (1.2.3)
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));
    REQUIRE(controller.screen() == Screen::UpdateAvailable);
    CHECK_FALSE(controller.updateCameFromLicenseView()); // auto-routed on startup

    controller.dismissUpdate();
    REQUIRE(controller.screen() == Screen::Details);

    controller.showUpdate(/*fromLicenseView*/ true); // e.g. the badge
    REQUIRE(controller.screen() == Screen::UpdateAvailable);
    CHECK(controller.updateCameFromLicenseView());

    controller.showUpdate(/*fromLicenseView*/ false); // e.g. auto open/focus
    CHECK_FALSE(controller.updateCameFromLicenseView());
}

TEST_CASE("revealing a missing installer reverts to the download state")
{
    controller_fixture fx;
    fx.config.applicationVersion = "2.3.1";
    ActivationController controller(fx.config, fx.makeLicensing());

    auto claims = default_claims();
    claims["p:rel"] = "2.4.0";
    auto lic = fx.makeLicensing()->validate_token_local_allow_expired(fx.token(claims));

    using Phase = ActivationController::UpdateInfo::Phase;
    controller.setPreviewUpdate(Phase::Done, lic); // no installer actually on disk
    REQUIRE(controller.updateInfo().phase == Phase::Done);

    controller.revealUpdateDownload(); // file is gone -> fall back to Ready
    CHECK(controller.updateInfo().phase == Phase::Ready);
    CHECK(controller.updateInfo().progress == doctest::Approx(0.0));
}

TEST_CASE("ActivationState stores ignored updates as JSON and preserves unknown keys")
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("moonbase-state-" + juce::Uuid().toString() + ".json");
    file.deleteFile();
    file.replaceWithText(R"({"futureKey":"keep-me"})"); // written by a "newer" build

    {
        ActivationState state(file);
        CHECK_FALSE(state.isUpdateIgnored("1.0.0"));
        state.ignoreUpdate("1.0.0");
        state.ignoreUpdate("1.0.0"); // idempotent
        state.ignoreUpdate("2.0.0");
        CHECK(state.isUpdateIgnored("1.0.0"));
        CHECK(state.ignoredUpdates().size() == 2);
        CHECK(state.get("futureKey").toString() == "keep-me"); // untouched
    }

    // Reload from disk: entries survived and the unknown key was not clobbered.
    ActivationState reloaded(file);
    CHECK(reloaded.isUpdateIgnored("1.0.0"));
    CHECK(reloaded.isUpdateIgnored("2.0.0"));
    CHECK(reloaded.get("futureKey").toString() == "keep-me");

    file.deleteFile();
}

TEST_CASE("start() with a valid offline license routes to Details")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["method"] = "Offline";
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->method == moonbase::activation_method::offline);
    CHECK(fx.transport->requests.empty()); // offline never contacts the API
}

TEST_CASE("start() deletes an expired offline license and locks to Welcome")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["method"] = "Offline";
    claims["exp"] = now_seconds() - 10; // already expired
    fx.seedStored(fx.token(claims));
    REQUIRE(fx.licenseFile.existsAsFile());

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Welcome);
    CHECK_FALSE(controller.license().has_value());
    CHECK(controller.lockReason() == LockReason::Expired);
    // The dead offline license can never be refreshed, so it is removed.
    CHECK_FALSE(fx.licenseFile.existsAsFile());
}

TEST_CASE("start() locks but keeps an untrusted token (wrong device) on disk")
{
    controller_fixture fx;
    auto claims = default_claims("some-other-device"); // sig won't match our fingerprint
    fx.seedRawToken(fx.token(claims));
    REQUIRE(fx.licenseFile.existsAsFile());

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Welcome);
    CHECK_FALSE(controller.license().has_value());
    CHECK(controller.lockReason() == LockReason::Invalid);
    // Not ours to delete - a foreign/tampered token is left untouched.
    CHECK(fx.licenseFile.existsAsFile());
}

//==============================================================================
// deactivate(): online revoke / local forget / unreachable
//==============================================================================
TEST_CASE("deactivate() on an online license revokes, clears, and removes the file")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(moonbase::http_response{200, {}, ""}); // revoke OK
    controller.deactivate();

    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));
    CHECK_FALSE(controller.license().has_value());
    CHECK_FALSE(controller.isBusy());
    CHECK_FALSE(fx.licenseFile.existsAsFile());
    CHECK(fx.transport->requests.size() == 1); // the revoke POST
    CHECK(controller.lockReason() == LockReason::Deactivated);
}

TEST_CASE("deactivate() on an offline license forgets locally without the API")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["method"] = "Offline";
    fx.seedStored(fx.token(claims));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    controller.deactivate(); // offline -> local forget, synchronous

    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));
    CHECK_FALSE(controller.license().has_value());
    CHECK_FALSE(fx.licenseFile.existsAsFile());
    CHECK(fx.transport->requests.empty());
    CHECK(controller.lockReason() == LockReason::Deactivated);
}

TEST_CASE("deactivate() that can't reach the server keeps the license and surfaces an error")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // No queued response -> recording_transport throws -> Unreachable.
    controller.deactivate();

    REQUIRE(pumpUntil([&] { return ! controller.isBusy(); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value()); // still licensed
    CHECK(controller.statusMessage().isNotEmpty());
    CHECK(fx.licenseFile.existsAsFile()); // not deleted
}

TEST_CASE("deactivate() keeps the license through a 404 anything could have sent")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // Stock ASP.NET NotFound(): Moonbase's for a license that is gone, but a
    // misrouted request gets the same from any other service. The seat may still
    // be taken, so the license stays and the user can try again.
    fx.transport->responses.push_back(moonbase::http_response{
        404, {}, R"({"type":"https://tools.ietf.org/html/rfc9110#section-15.5.5","title":"Not Found","status":404})"});
    controller.deactivate();

    REQUIRE(pumpUntil([&] { return ! controller.isBusy(); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value());
    CHECK(controller.statusMessage().contains("Couldn't reach Moonbase to deactivate"));
    CHECK(fx.licenseFile.existsAsFile());
}

TEST_CASE("deactivate() while Moonbase is rate limiting says to try again shortly")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(moonbase::http_response{429, {{"Retry-After", "60"}}, ""});
    controller.deactivate();

    REQUIRE(pumpUntil([&] { return ! controller.isBusy(); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value());
    CHECK(controller.statusMessage().containsIgnoreCase("Try again in a minute"));
    CHECK(fx.licenseFile.existsAsFile());
}

TEST_CASE("deactivate() keeps the license when the store has closed")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // The seat can't be freed, and nothing could be activated again, so
    // forgetting the license would only take it away from the buyer.
    fx.transport->responses.push_back(moonbase::http_response{
        410, {}, R"({"title":"Store closed","status":410,"detail":"This store has closed.","errorType":"StoreClosed"})"});
    controller.deactivate();

    REQUIRE(pumpUntil([&] { return ! controller.isBusy(); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value());
    CHECK(controller.licensedFlag().load());
    CHECK(controller.statusMessage().contains("This store has closed"));
    CHECK(fx.licenseFile.existsAsFile());
}

//==============================================================================
// Offline activation flow (machine file out, license file in)
//==============================================================================
TEST_CASE("saveOfflineRequest writes the device token to disk")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));

    auto requestFile = fx.licenseFile.getParentDirectory()
                           .getChildFile(juce::Uuid().toString() + ".dt");
    CHECK(controller.saveOfflineRequest(requestFile));
    CHECK(requestFile.existsAsFile());
    CHECK(requestFile.loadFileAsString().isNotEmpty());
    requestFile.deleteFile();
}

TEST_CASE("activateOffline with a valid license file unlocks and persists")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    // The license file the portal would have handed back for this device.
    auto claims = default_claims();
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getParentDirectory()
                            .getChildFile(juce::Uuid().toString() + ".mb");
    responseFile.replaceWithText(fx.token(claims));

    controller.setOfflineResponse(responseFile);
    controller.activateOffline();

    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Success; }));
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->method == moonbase::activation_method::offline);
    CHECK(fx.licenseFile.existsAsFile()); // persisted into the store
    CHECK(fx.transport->requests.empty());
    responseFile.deleteFile();
}

TEST_CASE("a license under a non-ASCII folder is saved where JUCE looks for it")
{
    // Regression: the controller built its store path from toStdString(), which
    // std::filesystem reads in the ANSI code page on Windows, so a user folder
    // like C:\Users\Björn sent the license somewhere else. Escaped UTF-8 keeps
    // the name independent of the compiler's source charset; the CJK letters are
    // outside every Western code page.
    controller_fixture fx;
    const juce::String folder(juce::CharPointer_UTF8("Bj\xc3\xb6rn \xe6\x97\xa5\xe6\x9c\xac"));
    fx.licenseFile = fx.licenseFile.getParentDirectory()
                         .getChildFile(juce::Uuid().toString() + " " + folder)
                         .getChildFile("license.mb");
    fx.config.licenseFile = fx.licenseFile;
    fx.config.deviceIdResolver = fx.fingerprint;

    // The config-only constructor, which builds the store from licenseFile.
    ActivationController controller(fx.config);
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    auto claims = default_claims();
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getSiblingFile("response.mb");
    REQUIRE(responseFile.replaceWithText(fx.token(claims)));

    controller.setOfflineResponse(responseFile);
    controller.activateOffline();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Success; }));

    // JUCE's view of the path is the ground truth: the store must have written
    // this exact file, not a mis-decoded twin of it.
    CHECK(fx.licenseFile.existsAsFile());

    // And a fresh controller reads it back from the same place.
    ActivationController relaunched(fx.config);
    relaunched.start();
    REQUIRE(pumpUntil([&] { return settled(relaunched); }));
    CHECK(relaunched.screen() == Screen::Details);
}

TEST_CASE("activateOffline with a foreign license file reports an error and stays offline")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    auto claims = default_claims("another-device"); // not for this fingerprint
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getParentDirectory()
                            .getChildFile(juce::Uuid().toString() + ".mb");
    responseFile.replaceWithText(fx.token(claims));

    controller.setOfflineResponse(responseFile);
    controller.activateOffline();

    REQUIRE(pumpUntil([&] { return controller.offlineError().isNotEmpty(); }));
    CHECK(controller.screen() == Screen::Offline);
    CHECK_FALSE(controller.license().has_value());
    CHECK_FALSE(fx.licenseFile.existsAsFile());
    responseFile.deleteFile();
}

//==============================================================================
// Misconfiguration: fail into an Error state, never throw out of construction
//==============================================================================
TEST_CASE("a missing public key fails into an Error state without throwing")
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";
    // publicKey deliberately left empty.
    config.licenseFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("moonbase-juce-tests")
                             .getChildFile(juce::Uuid().toString() + ".mb");
    juce::String captured;
    config.onDiagnostic = [&](const juce::String& m) { captured = m; };

    ActivationController controller(config); // primary ctor must not throw
    controller.start();

    CHECK(controller.screen() == Screen::Error);
    CHECK(controller.statusMessage().containsIgnoreCase("public key"));
    CHECK(captured.isNotEmpty());
    config.licenseFile.deleteFile();
}

TEST_CASE("a malformed public key is reported as a configuration error")
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";
    config.publicKey = "this-is-not-a-real-key";
    config.licenseFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("moonbase-juce-tests")
                             .getChildFile(juce::Uuid().toString() + ".mb");

    ActivationController controller(config); // SDK parses the key on construction
    controller.start();

    CHECK(controller.screen() == Screen::Error);
    CHECK(controller.statusMessage().containsIgnoreCase("configuration"));
    config.licenseFile.deleteFile();
}

TEST_CASE("diagnostics carry the underlying detail behind a friendly error")
{
    controller_fixture fx;
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    auto claims = default_claims("another-device"); // not for this device
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getParentDirectory()
                            .getChildFile(juce::Uuid().toString() + ".mb");
    responseFile.replaceWithText(fx.token(claims));

    controller.setOfflineResponse(responseFile);
    controller.activateOffline();

    REQUIRE(pumpUntil([&] { return controller.offlineError().isNotEmpty(); }));
    // The UI shows a friendly string; the diagnostic carries the real reason.
    CHECK(diags.size() >= 1);
    CHECK(diags.joinIntoString(" ").containsIgnoreCase("offline license file rejected"));
    responseFile.deleteFile();
}

namespace {
// Models a connect failure that carries developer guidance in the api_error
// detail field - exactly what juce_http_transport sets for the macOS sandbox
// network entitlement. Lets us assert the hint is routed to diagnostics only.
struct hinted_failure_transport : moonbase::http_transport
{
    moonbase::http_response send(const moonbase::http_request&) override
    {
        throw moonbase::api_error(
            0, "Couldn't connect to https://demo.moonbase.sh", {},
            "enable the com.apple.security.network.client entitlement");
    }
};
} // namespace

TEST_CASE("a transport failure routes the entitlement hint to diagnostics, not the user screen")
{
    controller_fixture fx;
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };

    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint,
        std::make_shared<hinted_failure_transport>());

    ActivationController controller(fx.config, licensing);
    controller.beginOnlineActivation();

    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Error; }));
    // The user sees a friendly, fixed prompt - never the entitlement detail.
    CHECK(controller.statusMessage().containsIgnoreCase("Couldn't reach Moonbase"));
    CHECK_FALSE(controller.statusMessage().containsIgnoreCase("entitlement"));
    // The developer sink gets the full reason, including the detail hint.
    CHECK(diags.joinIntoString(" ").containsIgnoreCase("entitlement"));
}

TEST_CASE("a failed activation start says what kind of failure it was")
{
    struct expectation
    {
        const char* name;
        std::optional<moonbase::http_response> response; // none: the connection fails
        const char* copy;
    };
    const std::vector<expectation> cases{
        {"connection failed", std::nullopt, "Couldn't reach Moonbase"},
        {"server error", moonbase::http_response{500, {}, ""}, "Try again in a minute"},
        {"rate limited", moonbase::http_response{429, {{"Retry-After", "60"}}, ""}, "Try again in a minute"},
        // A 404 or 403 is no verdict: a proxy or misrouted request sends them too.
        {"404", moonbase::http_response{404, {}, R"({"title":"Not Found","status":404})"}, "Couldn't reach Moonbase"},
        {"proxy 403", moonbase::http_response{403, {}, "<html><body>Blocked</body></html>"}, "Couldn't reach Moonbase"},
        {"product not active",
         moonbase::http_response{400, {}, R"({"title":"Invalid state","detail":"Product is not active","status":400})"},
         "isn't available for this product"},
        {"store closed",
         moonbase::http_response{410, {}, R"({"title":"Store closed","status":410,"detail":"This store has closed.","errorType":"StoreClosed"})"},
         "This store has closed"},
    };

    for (const auto& c : cases)
    {
        CAPTURE(c.name);
        controller_fixture fx;
        juce::StringArray diags;
        fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
        if (c.response)
            fx.transport->responses.push_back(*c.response);

        // A failed connection is an api_error with status 0 from both bundled
        // transports, which is what hinted_failure_transport throws.
        auto licensing = c.response ? fx.makeLicensing()
                                    : std::make_shared<moonbase::licensing>(
                                          fx.config.toLicensingOptions(), fx.store, fx.fingerprint,
                                          std::make_shared<hinted_failure_transport>());

        ActivationController controller(fx.config, licensing);
        controller.beginOnlineActivation();

        REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Error; }));
        CHECK(controller.statusMessage().contains(c.copy));
        CHECK(diags.joinIntoString(" ").contains("request_activation failed"));
    }
}

namespace {
// A resolver with a bug in it: the stand-in for any failure that is neither
// Moonbase's answer nor the connection's, such as the type_error nlohmann::json
// threw when a Windows computer name was read in the ANSI code page. The message
// is UTF-8, to check it is decoded as such on the way to the screen.
struct broken_resolver : moonbase::device_id_resolver
{
    std::string device_name() const override { return "Studio PC"; }
    std::string device_id() const override
    {
        throw std::runtime_error("resolver failed on Bj\xC3\xB6rn-PC");
    }
};
} // namespace

TEST_CASE("an unexpected failure starting activation shows what it was, not a connection problem")
{
    controller_fixture fx;
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, std::make_shared<broken_resolver>(), fx.transport);

    ActivationController controller(fx.config, licensing);
    controller.beginOnlineActivation();

    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Error; }));
    const auto reason = juce::String::fromUTF8("resolver failed on Bj\xC3\xB6rn-PC");
    CHECK(controller.statusMessage().contains(reason));
    CHECK_FALSE(controller.statusMessage().containsIgnoreCase("reach Moonbase"));
    CHECK_FALSE(controller.statusMessage().containsIgnoreCase("connection"));
    CHECK(diags.joinIntoString(" ").contains(reason));
    CHECK(fx.transport->requests.empty());
}

TEST_CASE("a machine file that can't be generated says why, not that the write failed")
{
    const auto requestFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("moonbase-juce-tests")
                                 .getChildFile(juce::Uuid().toString() + ".dt");

    SUBCASE("an unexpected failure shows its own words")
    {
        controller_fixture fx;
        juce::StringArray diags;
        fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
        auto licensing = std::make_shared<moonbase::licensing>(
            fx.config.toLicensingOptions(), fx.store, std::make_shared<broken_resolver>(), fx.transport);
        ActivationController controller(fx.config, licensing);

        CHECK_FALSE(controller.saveOfflineRequest(requestFile));
        const auto reason = juce::String::fromUTF8("resolver failed on Bj\xC3\xB6rn-PC");
        CHECK(controller.offlineError().contains(reason));
        CHECK_FALSE(controller.offlineError().containsIgnoreCase("write"));
        CHECK(diags.joinIntoString(" ").contains(reason));
    }

    SUBCASE("an unidentifiable machine is told so")
    {
        controller_fixture fx;
        struct no_identity : moonbase::device_id_resolver
        {
            std::string device_name() const override { return "Studio Mac"; }
            std::string device_id() const override
            {
                throw moonbase::insufficient_device_identity_error("test");
            }
        };
        auto licensing = std::make_shared<moonbase::licensing>(
            fx.config.toLicensingOptions(), fx.store, std::make_shared<no_identity>(), fx.transport);
        ActivationController controller(fx.config, licensing);

        CHECK_FALSE(controller.saveOfflineRequest(requestFile));
        CHECK(controller.offlineError().contains("can't be identified"));
    }

    CHECK_FALSE(requestFile.existsAsFile());
}

TEST_CASE("an unidentifiable machine is not told to check its connection")
{
    controller_fixture fx;
    struct no_identity : moonbase::device_id_resolver
    {
        std::string device_name() const override { return "Studio Mac"; }
        std::string device_id() const override
        {
            throw moonbase::insufficient_device_identity_error("test");
        }
    };
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, std::make_shared<no_identity>(), fx.transport);

    ActivationController controller(fx.config, licensing);
    controller.beginOnlineActivation();

    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Error; }));
    CHECK(controller.statusMessage().contains("can't be identified"));
    CHECK(fx.transport->requests.empty());
}

//==============================================================================
// Online re-validation (refresh entitlements after a purchase)
//==============================================================================
TEST_CASE("refreshLicense picks up newly granted sub-products from the server")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims())); // sp:owned = demo-app-pro,demo-app-extra
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->owned_sub_product_ids.size() == 2);
    CHECK(fx.transport->requests.empty()); // start() was within the throttle window

    // The server now returns a token that includes a just-purchased pack.
    auto upgraded = default_claims();
    upgraded["sp:owned"] = "demo-app-pro,demo-app-extra,demo-app-mega";
    fx.transport->responses.push_back(moonbase::http_response{200, {}, fx.token(upgraded)});

    bool done = false, ok = false;
    controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(ok);
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->owned_sub_product_ids.size() == 3); // entitlement refreshed
    CHECK(fx.transport->requests.size() == 1); // forced -> exactly one API round-trip

    // The request's User-Agent reports a real SDK version (not the unset 0.0.0
    // default) and identifies the JUCE module.
    const auto ua = fx.transport->requests.front().headers.at("User-Agent");
    CHECK(ua.find("moonbase-cpp/") == 0);
    CHECK(ua.find("moonbase-cpp/0.0.0") == std::string::npos);
    CHECK(ua.find("moonbase-juce/") != std::string::npos);
}

TEST_CASE("a refresh that lands after the license is cleared does not resurrect it")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    REQUIRE(fx.licenseFile.existsAsFile());

    // A forced refresh is in flight (server will return a valid token)...
    fx.transport->responses.push_back(moonbase::http_response{200, {}, fx.token(default_claims())});
    bool done = false, ok = true;
    controller.refreshLicense(true, [&](bool r) { done = true; ok = r; });

    // ...but the user deactivates/clears before it lands.
    controller.clearLicense();
    REQUIRE_FALSE(fx.licenseFile.existsAsFile());

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);                               // superseded result dropped
    CHECK_FALSE(controller.license().has_value()); // still cleared in memory
    CHECK_FALSE(fx.licenseFile.existsAsFile());    // and NOT recreated on disk
}

TEST_CASE("the module accepts a public key in every common text shape")
{
    // Runs on the native backend on Apple and Windows and on OpenSSL on Linux, so
    // together with the core suite every backend sees the same key strings.
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));

    const std::vector<std::pair<std::string, std::string>> formats{{"SPKI", fx.key.public_pem},
                                                                   {"PKCS#1", fx.key.public_pkcs1_pem}};
    for (const auto& format : formats)
    {
        for (const auto& shape : moonbase::tests::key_text_shapes(format.second))
        {
            CAPTURE(format.first);
            CAPTURE(shape.first);
            auto config = fx.config;
            config.publicKey = juce::String(shape.second);
            ActivationController controller(config, std::make_shared<moonbase::licensing>(
                                                        config.toLicensingOptions(), fx.store, fx.fingerprint,
                                                        fx.transport));
            controller.start();
            REQUIRE(pumpUntil([&] { return settled(controller); }));
            CHECK(controller.screen() == Screen::Details);
        }
    }
}

TEST_CASE("a public key with an out-of-bounds DER length is rejected cleanly")
{
    // Outer SEQUENCE(len 5) wrapping an INTEGER whose short-form length (0x7F)
    // claims 127 content bytes when only 3 remain. The parser must report a
    // configuration error, not read past the decoded buffer.
    const std::vector<unsigned char> der{0x30, 0x05, 0x02, 0x7F, 0x00, 0x00, 0x00};
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";
    config.publicKey = juce::String(moonbase::detail::base64_encode(der.data(), der.size()));
    config.licenseFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("moonbase-juce-tests")
                             .getChildFile(juce::Uuid().toString() + ".mb");

    ActivationController controller(config); // must not overrun / crash
    controller.start();
    CHECK(controller.screen() == Screen::Error);
    config.licenseFile.deleteFile();
}

TEST_CASE("refreshLicense keeps the current license when the server is unreachable")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // No queued response -> recording_transport throws -> refresh fails.
    bool done = false, ok = true;
    controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);
    CHECK(controller.license().has_value());      // not locked out by a blip
    CHECK(controller.screen() == Screen::Details);
}

TEST_CASE("refreshLicense locks when the server rejects the license for good")
{
    const std::vector<std::pair<const char*, moonbase::http_response>> rejections{
        {"license revoked",
         moonbase::http_response{400, {}, R"({"title":"Invalid state","detail":"License has been revoked","status":400,"errorType":"LicenseRevoked"})"}},
        {"subscription lapsed",
         moonbase::http_response{400, {}, R"({"title":"Invalid state","detail":"License has expired","status":400,"errorType":"LicenseExpired"})"}},
        {"license no longer active",
         moonbase::http_response{400, {}, R"({"title":"Invalid state","detail":"License is no longer active","status":400})"}},
        {"activation revoked",
         moonbase::http_response{400, {}, R"({"title":"Not allowed","detail":"License has been revoked","status":400,"errorType":"LicenseActivationRevoked"})"}},
    };

    for (const auto& entry : rejections)
    {
        CAPTURE(entry.first);
        const auto& response = entry.second;
        controller_fixture fx;
        juce::StringArray diags;
        fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
        fx.seedStored(fx.token(default_claims()));
        ActivationController controller(fx.config, fx.makeLicensing());
        controller.start();
        REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

        fx.transport->responses.push_back(response);
        bool done = false, ok = true;
        controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });

        REQUIRE(pumpUntil([&] { return done; }));
        CHECK_FALSE(ok);
        CHECK_FALSE(controller.license().has_value());
        CHECK(controller.screen() == Screen::Welcome);
        CHECK(fx.licenseFile.existsAsFile()); // kept, as start() keeps it, for the next launch to re-check
        CHECK(diags.joinIntoString(" ").contains("License rejected on re-validation"));
        // A lapsed subscription has ended; everything else was refused.
        CHECK(controller.lockReason()
              == (juce::String(entry.first) == "subscription lapsed" ? LockReason::Expired : LockReason::Invalid));
    }
}

namespace {
const moonbase::http_response storeClosed{
    410, {}, R"({"title":"Store closed","status":410,"detail":"This store has closed.","errorType":"StoreClosed"})"};
} // namespace

TEST_CASE("start() takes the offline license a closed store answers with, and never asks again")
{
    controller_fixture fx;
    auto online = default_claims();
    online.erase("exp");                                    // bought outright
    online["validated"] = now_seconds() - 60 * 60 * 24 * 365; // far past the grace period
    fx.seedStored(fx.token(online));

    // The same license as an offline one for this device, signed with the store's key.
    auto offline = online;
    offline["method"] = "Offline";
    offline["validated"] = now_seconds();
    offline.erase("p:rel");
    const auto offlineToken = fx.token(offline);
    fx.transport->responses.push_back(moonbase::http_response{200, {}, offlineToken});

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();

    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->method == moonbase::activation_method::offline);
    CHECK(controller.lockReason() == LockReason::None);
    CHECK(fx.store->load_local_license()->token == offlineToken);

    bool done = false;
    controller.refreshLicense(true, [&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(controller.license().has_value());
    CHECK(fx.transport->requests.size() == 1); // offline licenses are not re-validated
}

TEST_CASE("a closed store's StoreClosed reads like being offline")
{
    SUBCASE("within the grace period, refreshLicense keeps the license")
    {
        controller_fixture fx;
        auto claims = default_claims();
        claims["trial"] = true; // a trial's validation is one of the calls a closed store refuses
        fx.seedStored(fx.token(claims));
        ActivationController controller(fx.config, fx.makeLicensing());
        controller.start();
        REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));

        fx.transport->responses.push_back(storeClosed);
        bool done = false, ok = true;
        controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });

        REQUIRE(pumpUntil([&] { return done; }));
        CHECK_FALSE(ok);
        CHECK(controller.license().has_value());
        CHECK(controller.screen() == Screen::Trial);
        CHECK(fx.licenseFile.existsAsFile());
    }

    SUBCASE("past it, start() locks as unverified and keeps the file")
    {
        controller_fixture fx;
        fx.config.onlineGracePeriod = std::chrono::seconds(20);
        auto claims = default_claims();
        claims["trial"] = true;
        claims["validated"] = now_seconds() - 60;
        fx.seedStored(fx.token(claims));
        fx.transport->responses.push_back(storeClosed);

        ActivationController controller(fx.config, fx.makeLicensing());
        controller.start();

        REQUIRE(pumpUntil([&] { return settled(controller); }));
        CHECK(controller.screen() == Screen::Welcome);
        CHECK_FALSE(controller.license().has_value());
        CHECK(controller.lockReason() == LockReason::Unverified);
        CHECK(fx.licenseFile.existsAsFile());
    }
}

TEST_CASE("refreshLicense keeps the license through what only looks like a verdict")
{
    const std::vector<std::pair<const char*, moonbase::http_response>> blips{
        {"captive portal", moonbase::http_response{200, {}, "<html><body>Sign in</body></html>"}},
        {"proxy 404", moonbase::http_response{404, {}, ""}},
        {"404 ProblemDetails", moonbase::http_response{404, {}, R"({"title":"Not Found","status":404})"}},
        {"rate limited", moonbase::http_response{429, {{"Retry-After", "60"}}, ""}},
        {"gateway timeout", moonbase::http_response{504, {}, R"({"message":"Endpoint request timed out"})"}},
    };

    for (const auto& entry : blips)
    {
        CAPTURE(entry.first);
        const auto& response = entry.second;
        controller_fixture fx;
        fx.seedStored(fx.token(default_claims()));
        ActivationController controller(fx.config, fx.makeLicensing());
        controller.start();
        REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

        fx.transport->responses.push_back(response);
        bool done = false, ok = true;
        controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });

        REQUIRE(pumpUntil([&] { return done; }));
        CHECK_FALSE(ok);
        CHECK(controller.license().has_value());
        CHECK(controller.screen() == Screen::Details);
    }
}

TEST_CASE("refreshLicense locks once the grace period has run out and Moonbase can't be reached")
{
    controller_fixture fx;
    fx.config.onlineGracePeriod = std::chrono::seconds(20);
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
    auto claims = default_claims();
    claims["validated"] = now_seconds() - 30; // past the grace period, so start() checks online
    fx.seedStored(fx.token(claims));

    // Moonbase answers start(), but its clock runs a little behind ours, so the
    // license it returns is already past the grace period here.
    auto behind = default_claims();
    behind["validated"] = now_seconds() - 25;
    fx.transport->responses.push_back(moonbase::http_response{200, {}, fx.token(behind)});

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // Nothing queued: the forced re-check can't reach Moonbase.
    bool done = false, ok = true;
    controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });
    REQUIRE(pumpUntil([&] { return done; }));

    CHECK_FALSE(ok);
    CHECK_FALSE(controller.licensedFlag().load());
    CHECK(controller.screen() == Screen::Welcome);
    CHECK(controller.lockReason() == LockReason::Unverified);
    CHECK(fx.licenseFile.existsAsFile()); // kept for the next launch, as start() keeps it
    CHECK(diags.joinIntoString(" ").contains("offline grace period"));
}

//==============================================================================
// License watch: expiry and other instances, without a restart
//==============================================================================
TEST_CASE("a trial that ends while the plugin is open locks without a restart")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    claims["exp"] = now_seconds() + 2;
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));
    REQUIRE(controller.licensedFlag().load());

    CHECK(pumpUntil([&] { return controller.screen() == Screen::Expired; }, 10000));
    CHECK_FALSE(controller.licensedFlag().load());
    REQUIRE(controller.expiredTrial().has_value());
    CHECK(controller.lockReason() == LockReason::Expired);
    CHECK(fx.transport->requests.empty()); // a passed exp is decided locally
}

TEST_CASE("a subscription that ends while the plugin is open locks and says it expired")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["exp"] = now_seconds() + 2; // a full license with an end date
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    CHECK(controller.lockReason() == LockReason::None);

    CHECK(pumpUntil([&] { return controller.screen() == Screen::Welcome; }, 10000));
    CHECK_FALSE(controller.licensedFlag().load());
    CHECK(controller.lockReason() == LockReason::Expired);
    CHECK(fx.transport->requests.empty()); // a passed exp is decided locally
}

TEST_CASE("a license offline past its grace period locks while the plugin is open")
{
    controller_fixture fx;
    fx.config.onlineGracePeriod = std::chrono::seconds(3);
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
    auto claims = default_claims();
    claims["validated"] = now_seconds(); // fresh, so start() stays local
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    CHECK(fx.transport->requests.empty());

    // Nothing queued: once the grace period ends, Moonbase can't be reached.
    CHECK(pumpUntil([&] { return controller.screen() == Screen::Welcome; }, 10000));
    CHECK_FALSE(controller.licensedFlag().load());
    CHECK(controller.lockReason() == LockReason::Unverified);
    CHECK(fx.transport->requests.size() == 1); // one last attempt, then locked
    CHECK(diags.joinIntoString(" ").contains("offline grace period"));
    CHECK(fx.licenseFile.existsAsFile());
}

TEST_CASE("an activation in another instance unlocks this one without a reload")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    fx.seedStored(fx.token(default_claims())); // what the other instance writes on activation

    CHECK(pumpUntil([&] { return controller.screen() == Screen::Details; }, 6000));
    CHECK(controller.licensedFlag().load());
    CHECK(fx.transport->requests.empty()); // validated locally, no network
}

TEST_CASE("a sibling's re-validation updates this instance without moving its screen")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    controller.showOffline(); // somewhere a re-route would move it away from

    auto upgraded = default_claims();
    upgraded["sp:owned"] = "demo-app-pro,demo-app-extra,demo-app-mega";
    fx.seedStored(fx.token(upgraded));

    CHECK(pumpUntil([&] { return controller.license()
                                 && controller.license()->owned_sub_product_ids.size() == 3; },
                    6000));
    CHECK(controller.screen() == Screen::Offline);
}

TEST_CASE("a license file caught mid-write is left alone and read again")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // Another writer has truncated the file and written only half of it so far.
    const auto full = fx.licenseFile.loadFileAsString();
    REQUIRE(fx.licenseFile.replaceWithText(full.substring(0, full.length() / 2)));
    pumpFor(4500); // two watch ticks
    CHECK(controller.licensedFlag().load());
    CHECK(fx.licenseFile.existsAsFile()); // not deleted as corrupt

    // The write completes, with a newer license this instance then picks up.
    auto upgraded = default_claims();
    upgraded["sp:owned"] = "demo-app-pro,demo-app-extra,demo-app-mega";
    fx.seedStored(fx.token(upgraded));
    CHECK(pumpUntil([&] { return controller.license()
                                 && controller.license()->owned_sub_product_ids.size() == 3; },
                    6000));
}

TEST_CASE("deactivating in another instance locks this one too")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    ActivationController other(fx.config, fx.makeLicensing());
    controller.start();
    other.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details && other.screen() == Screen::Details; }));

    other.clearLicense(); // removes the shared license file
    CHECK(other.lockReason() == LockReason::Deactivated);

    CHECK(pumpUntil([&] { return controller.screen() == Screen::Welcome; }, 6000));
    CHECK_FALSE(controller.licensedFlag().load());
    CHECK(controller.lockReason() == LockReason::Deactivated);

    // Activated again elsewhere: licensed, so nothing left to explain.
    auto again = default_claims();
    again["id"] = "activation-456";
    fx.seedStored(fx.token(again));
    CHECK(pumpUntil([&] { return controller.screen() == Screen::Details; }, 6000));
    CHECK(controller.lockReason() == LockReason::None);
}

TEST_CASE("a license that couldn't be saved is not locked by the watch")
{
    // Activated, but the license file couldn't be written: the license stays
    // unlocked for the session, and a file that never existed is not mistaken
    // for one another instance removed.
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));
    REQUIRE(fx.licenseFile.createDirectory()); // a folder in the way: every save fails

    auto claims = default_claims();
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getParentDirectory().getChildFile(juce::Uuid().toString() + ".mb");
    responseFile.replaceWithText(fx.token(claims));
    controller.setOfflineResponse(responseFile);
    controller.activateOffline();
    REQUIRE(controller.licensedFlag().load());

    pumpFor(4500); // two watch ticks
    CHECK(controller.licensedFlag().load());
    CHECK(controller.screen() == Screen::Success);
    responseFile.deleteFile();
    fx.licenseFile.deleteRecursively();
}

TEST_CASE("a stored license past its grace period is not picked up without a server check")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    // Signed, for this device and unexpired, but last verified 8 days ago: past
    // the default 7-day grace period.
    auto stale = default_claims();
    stale["validated"] = now_seconds() - 8 * 24 * 3600;
    fx.seedStored(fx.token(stale));

    // Not even for a moment: two watch ticks, and it must never unlock.
    CHECK_FALSE(pumpUntil([&] { return controller.licensedFlag().load(); }, 4500));
    CHECK(controller.screen() == Screen::Welcome);
    CHECK(fx.transport->requests.empty());
}

TEST_CASE("a replacement activation from another instance is not undone by an older refresh")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims())); // activation-123
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // A refresh of activation-123 is on its way; by the time it answers, that
    // activation has been revoked...
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{
            400, {}, R"({"title":"Not allowed","detail":"License has been revoked","status":400,"errorType":"LicenseActivationRevoked"})"});
    }
    bool done = false, ok = true;
    controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));

    // ...because another instance re-activated this machine as activation-456.
    // It stores that under the store's lock, which this refresh holds from its
    // request until it is answered, so its write comes after the answer.
    auto replacement = default_claims();
    replacement["id"] = "activation-456";
    const auto replacementToken = fx.token(replacement);
    std::thread otherInstance([&] { fx.seedStored(replacementToken); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // waiting on the lock
    gated->release();
    otherInstance.join();

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok); // the old activation was refused...
    // ...and the replacement is picked up from the file after it.
    REQUIRE(pumpUntil([&] { return controller.license() && controller.license()->activation_id == "activation-456"; },
                      6000));
    CHECK(controller.licensedFlag().load());
    auto stored = fx.store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->activation_id == "activation-456"); // nor wrote the old one back
}

TEST_CASE("the license watch does not supersede a refresh the host asked for")
{
    controller_fixture fx;
    fx.config.onlineGracePeriod = std::chrono::seconds(3);
    auto claims = default_claims();
    claims["validated"] = now_seconds(); // fresh, so start() stays local
    fx.seedStored(fx.token(claims));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // The host re-checks after a purchase; the answer takes a while, long
    // enough for the grace period to run out in the meantime.
    auto upgraded = default_claims();
    upgraded["sp:owned"] = "demo-app-pro,demo-app-extra,demo-app-mega";
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{200, {}, fx.token(upgraded)});
    }
    bool done = false, ok = false;
    controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));
    pumpFor(5000); // past the grace period, two watch ticks

    gated->release();
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(ok); // the host's refresh landed
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->owned_sub_product_ids.size() == 3);
    CHECK(gated->requests.load() == 1); // and the watch didn't start a second one over it
}

TEST_CASE("a license removed elsewhere during an activation locks but leaves the activation running")
{
    controller_fixture fx;
    fx.config.openBrowser = [](const juce::URL&) { return true; };
    auto claims = default_claims();
    claims["trial"] = true;
    fx.seedStored(fx.token(claims));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));

    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"});
    controller.beginOnlineActivation(); // "Unlock"
    REQUIRE(pumpUntil([&] { return controller.pendingBrowserUrl().isNotEmpty(); }));

    {
        auto guard = fx.store->lock_for_update(); // another instance forgets the license
        fx.store->delete_local_license();
    }

    CHECK(pumpUntil([&] { return ! controller.licensedFlag().load(); }, 6000));
    CHECK(controller.screen() == Screen::BrowserWait);  // the flow is still on screen...
    CHECK(controller.pendingBrowserUrl().isNotEmpty()); // ...and still waiting
    controller.cancelActivation();
    CHECK(controller.screen() == Screen::Welcome);
}

TEST_CASE("onLicenseChanged reports the settled state, then only license changes")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    std::vector<bool> reports;
    controller.onLicenseChanged = [&](bool licensed) { reports.push_back(licensed); };
    controller.start();
    REQUIRE(pumpUntil([&] { return reports.size() == 1; }));
    CHECK(reports.front());

    // Navigation and a re-check that changes nothing are not license changes.
    controller.showOffline();
    controller.showDetails();
    bool done = false;
    controller.refreshLicense(false, [&](bool) { done = true; }); // within the throttle: same token
    REQUIRE(pumpUntil([&] { return done; }));
    pumpFor(200);
    CHECK(reports.size() == 1);

    // A refresh that brings a new token is.
    auto upgraded = default_claims();
    upgraded["sp:owned"] = "demo-app-pro,demo-app-extra,demo-app-mega";
    fx.transport->responses.push_back(moonbase::http_response{200, {}, fx.token(upgraded)});
    done = false;
    controller.refreshLicense(true, [&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done && reports.size() == 2; }));
    CHECK(reports.back());

    controller.clearLicense();
    REQUIRE(pumpUntil([&] { return reports.size() == 3; }));
    CHECK_FALSE(reports.back());
}

TEST_CASE("onLicenseChanged reports an unlicensed start once")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    std::vector<bool> reports;
    controller.onLicenseChanged = [&](bool licensed) { reports.push_back(licensed); };
    controller.start();
    REQUIRE(pumpUntil([&] { return ! reports.empty(); }));
    controller.showOffline();
    controller.showWelcome();
    pumpFor(200);
    CHECK(reports == std::vector<bool>{false});
}

TEST_CASE("the browser link reaches a custom UI, which is told when it arrives")
{
    controller_fixture fx;
    juce::StringArray opened;
    fx.config.openBrowser = [&](const juce::URL& url) { opened.add(url.toString(true)); return false; };
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));
    CHECK(controller.pendingBrowserUrl().isEmpty());

    // What a custom UI does: listen for changes and read the link.
    struct LinkWatcher : juce::ChangeListener
    {
        explicit LinkWatcher(ActivationController& c) : controller(c) {}
        void changeListenerCallback(juce::ChangeBroadcaster*) override { link = controller.pendingBrowserUrl(); }
        ActivationController& controller;
        juce::String link;
    } watcher(controller);
    controller.addChangeListener(&watcher);

    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"});
    controller.beginOnlineActivation();

    CHECK(pumpUntil([&] { return watcher.link.isNotEmpty(); }));
    CHECK(watcher.link.contains("token=request-123"));
    CHECK(opened == juce::StringArray{watcher.link}); // through the host's hook, not the system browser
    CHECK(diags.joinIntoString(" ").contains("pendingBrowserUrl()")); // the hook said it couldn't open it
    CHECK(controller.browserOpenFailed()); // so the screen offers the link to copy

    controller.cancelActivation();
    CHECK(controller.pendingBrowserUrl().isEmpty());
    CHECK_FALSE(controller.browserOpenFailed());
    controller.removeChangeListener(&watcher);
}

TEST_CASE("browserOpenFailed is only set while a link the browser couldn't open is waiting")
{
    controller_fixture fx;
    bool canOpen = true;
    fx.config.openBrowser = [&](const juce::URL&) { return canOpen; };
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));
    CHECK_FALSE(controller.browserOpenFailed());

    const auto requestResponse = [] {
        return moonbase::http_response{
            200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"};
    };

    // The browser opened: nothing to flag.
    fx.transport->responses.push_back(requestResponse());
    controller.beginOnlineActivation();
    REQUIRE(pumpUntil([&] { return controller.pendingBrowserUrl().isNotEmpty(); }));
    CHECK_FALSE(controller.browserOpenFailed());
    controller.cancelActivation();

    // It didn't (no default browser): flagged until the activation ends.
    canOpen = false;
    fx.transport->responses.push_back(requestResponse());
    controller.beginOnlineActivation();
    REQUIRE(pumpUntil([&] { return controller.pendingBrowserUrl().isNotEmpty(); }));
    CHECK(controller.browserOpenFailed());
    controller.cancelActivation();
    CHECK_FALSE(controller.browserOpenFailed());

    // A later attempt that opens clears it.
    canOpen = true;
    fx.transport->responses.push_back(requestResponse());
    controller.beginOnlineActivation();
    REQUIRE(pumpUntil([&] { return controller.pendingBrowserUrl().isNotEmpty(); }));
    CHECK_FALSE(controller.browserOpenFailed());
    controller.cancelActivation();
}

TEST_CASE("a previewed browser wait carries its link, and the next preview drops it")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.setPreviewState(Screen::BrowserWait);
    CHECK(controller.pendingBrowserUrl().isEmpty());

    controller.setPreviewBrowserLink("https://demo.moonbase.sh/activate?token=preview", false);
    CHECK(controller.screen() == Screen::BrowserWait);
    CHECK(controller.pendingBrowserUrl() == "https://demo.moonbase.sh/activate?token=preview");
    CHECK(controller.browserOpenFailed());

    controller.setPreviewState(Screen::Welcome);
    CHECK(controller.pendingBrowserUrl().isEmpty());
    CHECK_FALSE(controller.browserOpenFailed());
}

TEST_CASE("a trial that ends while it is being unlocked still locks, and the activation carries on")
{
    controller_fixture fx;
    fx.config.openBrowser = [](const juce::URL&) { return true; };
    auto claims = default_claims();
    claims["trial"] = true;
    claims["exp"] = now_seconds() + 2;
    fx.seedStored(fx.token(claims));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));

    // "Unlock": start an online activation and leave it waiting (no poll answers).
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"});
    controller.beginOnlineActivation();
    REQUIRE(pumpUntil([&] { return controller.pendingBrowserUrl().isNotEmpty(); }));
    REQUIRE(controller.licensedFlag().load());

    CHECK(pumpUntil([&] { return ! controller.licensedFlag().load(); }, 10000));
    CHECK(controller.screen() == Screen::BrowserWait);       // the flow was left alone
    CHECK(controller.pendingBrowserUrl().isNotEmpty());      // and is still waiting
    CHECK_FALSE(controller.license().has_value());

    controller.cancelActivation();
    CHECK(controller.screen() == Screen::Welcome);
}

TEST_CASE("refreshLicense leaves an activation in progress alone")
{
    controller_fixture fx;
    fx.config.openBrowser = [](const juce::URL&) { return true; };
    auto claims = default_claims();
    claims["trial"] = true;
    fx.seedStored(fx.token(claims));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));

    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"});
    controller.beginOnlineActivation();

    // Straight away, before the request has even come back: what a host does
    // when another editor opens.
    bool done = false, ok = true;
    controller.refreshLicense(true, [&](bool refreshed) { done = true; ok = refreshed; });
    CHECK(done);
    CHECK_FALSE(ok);

    CHECK(pumpUntil([&] { return controller.pendingBrowserUrl().isNotEmpty(); })); // the request still landed
    CHECK(controller.screen() == Screen::BrowserWait);
    controller.cancelActivation();
}

//==============================================================================
// Validation / network tuning
//==============================================================================
TEST_CASE("validation + timeout tuning flows into the SDK options")
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";

    // Defaults match the SDK.
    CHECK(config.toLicensingOptions().online_validation_min_interval == std::chrono::minutes(5));
    CHECK(config.toLicensingOptions().online_validation_grace_period == std::chrono::hours(24 * 7));

    config.onlineCheckInterval = std::chrono::hours(1);
    config.onlineGracePeriod = std::chrono::hours(24 * 30);
    config.httpConnectTimeout = std::chrono::seconds(3);
    config.httpRequestTimeout = std::chrono::seconds(8);

    const auto opts = config.toLicensingOptions();
    CHECK(opts.online_validation_min_interval == std::chrono::hours(1));
    CHECK(opts.online_validation_grace_period == std::chrono::hours(24 * 30));
    CHECK(opts.http_connect_timeout == std::chrono::seconds(3));
    CHECK(opts.http_request_timeout == std::chrono::seconds(8));
}

//==============================================================================
// Telemetry / analytics metadata
//==============================================================================
TEST_CASE("the JUCE module identifies itself via client_info (User-Agent)")
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";

    const auto opts = config.toLicensingOptions();
    REQUIRE(opts.client_info.has_value());
    CHECK(opts.client_info->find("moonbase-juce/") != std::string::npos);
    CHECK(opts.client_info->find("JUCE") != std::string::npos); // JUCE version
    CHECK_FALSE(opts.client_info->empty());
}

TEST_CASE("a consumer's clientInfo is appended after the module's own segment")
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";

    // Unset: the module's own segment only, which ends with the "(JUCE …; OS)" comment.
    CHECK(config.resolvedClientInfo().startsWith("moonbase-juce/"));
    CHECK(config.resolvedClientInfo().endsWith(")"));

    // Whitespace-only reads as unset, so no dangling separator.
    config.clientInfo = "   ";
    CHECK(config.resolvedClientInfo().endsWith(")"));

    // Set: appended last, and that is exactly what reaches the SDK options.
    config.clientInfo = "HISE/4.1.0";
    const auto resolved = config.resolvedClientInfo();
    CHECK(resolved.startsWith("moonbase-juce/"));
    CHECK(resolved.endsWith(" HISE/4.1.0"));
    CHECK(config.toLicensingOptions().client_info == resolved.toStdString());

    // Layers append rather than assign, so each one keeps its mark.
    config.clientInfo << " MyWrapper/2.0";
    CHECK(config.resolvedClientInfo().endsWith(" HISE/4.1.0 MyWrapper/2.0"));
}

TEST_CASE("clientInfo reaches the wire and cannot inject a header")
{
    controller_fixture fx;
    fx.config.clientInfo = "HISE/4.1.0\r\nX-Injected: 1";
    fx.seedStored(fx.token(default_claims()));

    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    REQUIRE(fx.transport->requests.empty()); // start() was within the throttle window

    fx.transport->responses.push_back(moonbase::http_response{200, {}, fx.token(default_claims())});
    bool done = false;
    controller.refreshLicense(true, [&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));

    REQUIRE(fx.transport->requests.size() == 1);
    const auto ua = fx.transport->requests.front().headers.at("User-Agent");
    CHECK(ua.find("moonbase-cpp/") == 0);
    CHECK(ua.find("moonbase-juce/") < ua.find("HISE/4.1.0")); // module segment first
    CHECK(ua.find('\r') == std::string::npos);
    CHECK(ua.find('\n') == std::string::npos);
}

TEST_CASE("analytics capture is off by default and easy to switch on")
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";

    // Off by default: no metadata leaves the building.
    CHECK(config.toLicensingOptions().metadata.empty());

    // One flag turns on the JUCE system capture; static + hook metadata merge in.
    config.analytics.enabled = true;
    config.metadata["app.channel"] = "beta";
    config.onCollectMetadata = [](std::map<std::string, std::string>& m) { m["cohort"] = "A"; };

    const auto opts = config.toLicensingOptions();
    CHECK(opts.metadata.count("juce.os.is64Bit") == 1); // always emplaced by the capture
    CHECK(opts.metadata.count("juce.cpu.cores") == 1);
    CHECK(opts.metadata.at("app.channel") == "beta"); // explicit metadata preserved
    CHECK(opts.metadata.at("cohort") == "A");          // last-word hook ran
}

//==============================================================================
// Plugin integration helpers
//==============================================================================
TEST_CASE("licensedFlag mirrors the license state for the audio thread")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    CHECK_FALSE(controller.licensedFlag().load()); // not set until start() resolves

    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    CHECK(controller.licensedFlag().load());

    fx.transport->responses.push_back(moonbase::http_response{200, {}, ""}); // revoke OK
    controller.deactivate();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));
    CHECK_FALSE(controller.licensedFlag().load());
}

TEST_CASE("ActivationComponent can share an externally-owned controller")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    // One controller, shared - no second instance, no hand-rolled re-sync.
    CHECK(&component.controller() == &shared);
    CHECK(component.controller().screen() == Screen::Details);
    CHECK(component.controller().licensedFlag().load());
}

TEST_CASE("a component sharing a settled controller reports its state after construction")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    std::vector<bool> reports;
    component.onActivationChanged = [&](bool active) { reports.push_back(active); }; // wired after the ctor
    REQUIRE(pumpUntil([&] { return ! reports.empty(); }));
    CHECK(reports == std::vector<bool>{true});

    // Navigation is not an activation change.
    shared.showOffline();
    shared.showDetails();
    pumpFor(200);
    CHECK(reports.size() == 1);

    shared.clearLicense();
    REQUIRE(pumpUntil([&] { return reports.size() == 2; }));
    CHECK_FALSE(reports.back());
}

//==============================================================================
// ActivationComponent: presenting itself when the plugin locks
//==============================================================================
TEST_CASE("a dismissed overlay comes back when the license is lost")
{
    controller_fixture fx;
    fx.config.reduceMotion = true; // appear() and dismiss() take effect at once
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    // Hidden, as after the user closed it. No onActivationChanged: presenting
    // must not depend on the host wiring one.
    ActivationComponent component(shared);
    pumpFor(200);
    CHECK_FALSE(component.isVisible()); // licensed: nothing to present

    shared.clearLicense();
    CHECK(pumpUntil([&] { return component.isVisible(); }));

    // Once per lock: moving around while still locked leaves the host in charge.
    component.dismiss();
    shared.showOffline();
    shared.showWelcome();
    pumpFor(200);
    CHECK_FALSE(component.isVisible());
}

TEST_CASE("an overlay opened on a locked plugin presents itself")
{
    controller_fixture fx;
    fx.config.reduceMotion = true;
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Welcome; }));

    ActivationComponent component(shared); // e.g. an editor opened after the lock
    CHECK_FALSE(component.isVisible());
    CHECK(pumpUntil([&] { return component.isVisible(); }));
}

TEST_CASE("a trial that ends while the plugin is open presents the overlay")
{
    controller_fixture fx;
    fx.config.reduceMotion = true;
    auto claims = default_claims();
    claims["trial"] = true;
    claims["exp"] = now_seconds() + 2;
    fx.seedStored(fx.token(claims));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Trial; }));

    ActivationComponent component(shared);
    pumpFor(200);
    REQUIRE_FALSE(component.isVisible());

    CHECK(pumpUntil([&] { return component.isVisible(); }, 10000));
    CHECK(shared.screen() == Screen::Expired);
}

TEST_CASE("an overlay that opened for a lock closes again when the license comes back")
{
    controller_fixture fx;
    fx.config.reduceMotion = true;
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    ActivationController other(fx.config, fx.makeLicensing()); // another plugin instance
    shared.start();
    other.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details && other.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    pumpFor(200);
    REQUIRE_FALSE(component.isVisible());

    other.clearLicense();
    REQUIRE(pumpUntil([&] { return component.isVisible(); }, 6000));
    CHECK(shared.lockReason() == LockReason::Deactivated);

    // Activated again in the other instance: nothing left to do here.
    auto again = default_claims();
    again["id"] = "activation-456";
    fx.seedStored(fx.token(again));
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }, 6000));
    CHECK(pumpUntil([&] { return ! component.isVisible(); }));
}

TEST_CASE("an overlay the user had open stays up when the license comes back")
{
    controller_fixture fx;
    fx.config.reduceMotion = true;
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    ActivationController other(fx.config, fx.makeLicensing());
    shared.start();
    other.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details && other.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    component.appear(); // the user opened the license view
    pumpFor(200);

    other.clearLicense();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Welcome; }, 6000));
    CHECK(component.isVisible());

    auto again = default_claims();
    again["id"] = "activation-456";
    fx.seedStored(fx.token(again));
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }, 6000));
    pumpFor(200);
    CHECK(component.isVisible()); // not ours to close
}

TEST_CASE("an overlay that opened for a lock keeps the success screen of an activation in it")
{
    controller_fixture fx;
    fx.config.reduceMotion = true;
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    pumpFor(200);
    shared.clearLicense();
    REQUIRE(pumpUntil([&] { return component.isVisible(); }));

    // The user activates right there (offline, so no network is involved).
    auto claims = default_claims();
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getParentDirectory().getChildFile(juce::Uuid().toString() + ".mb");
    responseFile.replaceWithText(fx.token(claims));
    shared.setOfflineResponse(responseFile);
    shared.activateOffline();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Success; }));
    pumpFor(200);
    CHECK(component.isVisible()); // "Open {product}" is the user's to press
    responseFile.deleteFile();
}

TEST_CASE("an overlay closed on the success screen opens on the license next time")
{
    controller_fixture fx;
    fx.config.reduceMotion = true; // appear() and dismiss() take effect at once
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Welcome; }));

    ActivationComponent component(shared);
    component.onClose = [&] { component.dismiss(); };
    REQUIRE(pumpUntil([&] { return component.isVisible(); }));

    auto claims = default_claims();
    claims["method"] = "Offline";
    auto responseFile = fx.licenseFile.getParentDirectory().getChildFile(juce::Uuid().toString() + ".mb");
    responseFile.replaceWithText(fx.token(claims));
    shared.setOfflineResponse(responseFile);
    shared.activateOffline();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Success; }));

    // "Open {product}", as the success screen's button does.
    component.onClose();
    CHECK_FALSE(component.isVisible());
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    component.appear();
    pumpFor(100);
    CHECK(component.isVisible());
    CHECK(shared.screen() == Screen::Details);
    responseFile.deleteFile();
}

TEST_CASE("autoPresentOnLock=false leaves presenting to the host")
{
    controller_fixture fx;
    fx.config.reduceMotion = true;
    fx.config.autoPresentOnLock = false;
    fx.seedStored(fx.token(default_claims()));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    std::vector<bool> reports;
    component.onActivationChanged = [&](bool active) { reports.push_back(active); };
    REQUIRE(pumpUntil([&] { return reports.size() == 1; }));

    shared.clearLicense();
    REQUIRE(pumpUntil([&] { return reports.size() == 2; }));
    CHECK_FALSE(reports.back()); // the host still hears about it
    CHECK_FALSE(component.isVisible());
}

TEST_CASE("a lock does not replay the appear animation on an overlay already up")
{
    controller_fixture fx; // motion on: appear() would restart the fade from 0
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Welcome; }));

    ActivationComponent component(shared);
    component.setVisible(true); // as with addAndMakeVisible
    std::optional<float> alphaAtReport;
    component.onActivationChanged = [&](bool) { alphaAtReport = component.getAlpha(); };
    REQUIRE(pumpUntil([&] { return alphaAtReport.has_value(); }));
    CHECK(*alphaAtReport == doctest::Approx(1.0f));
    CHECK(component.isVisible());
}

TEST_CASE("LicenseGate gates click-free: pass-through licensed, ramp to silence unlicensed")
{
    LicenseGate gate;
    gate.prepare(48000.0, 5.0); // ~240-sample fade
    gate.reset(true);           // start open

    std::vector<float> data(256, 1.0f);
    float* chans[1] = { data.data() };

    // Licensed: untouched.
    gate.process(chans, 1, 256, true);
    CHECK(data.front() == doctest::Approx(1.0f));
    CHECK(data.back() == doctest::Approx(1.0f));

    // Unlicensed: ramps down (not an instant cut) and reaches silence by block end.
    for (auto& s : data) s = 1.0f;
    gate.process(chans, 1, 256, false);
    CHECK(data.front() < 1.0f);
    CHECK(data.front() > 0.0f);
    CHECK(data.back() < data.front());
    CHECK(gate.currentGain() == doctest::Approx(0.0f));

    // Fully closed: buffer cleared.
    for (auto& s : data) s = 1.0f;
    gate.process(chans, 1, 256, false);
    CHECK(data.front() == doctest::Approx(0.0f));
    CHECK(data.back() == doctest::Approx(0.0f));
}

TEST_CASE("setPreviewState routes the error to the field the screen renders")
{
    controller_fixture fx;
    ActivationController controller(fx.config, fx.makeLicensing());

    // Welcome/Error + Details read statusMessage().
    controller.setPreviewState(Screen::Error, std::nullopt, "could not reach the server");
    CHECK(controller.statusMessage() == "could not reach the server");
    CHECK(controller.offlineError().isEmpty());

    // The offline view reads offlineError().
    controller.setPreviewState(Screen::Offline, std::nullopt, "that file isn't valid");
    CHECK(controller.offlineError() == "that file isn't valid");
    CHECK(controller.statusMessage().isEmpty());
}

//==============================================================================
// Teardown / lifetime
//==============================================================================
namespace {
// A transport whose send() blocks until cancel() is called, to simulate a
// request in flight when the controller is destroyed (plugin scan / rapid close).
struct blocking_transport : moonbase::http_transport
{
    juce::WaitableEvent gate;
    std::atomic<bool> entered{ false };

    moonbase::http_response send(const moonbase::http_request&) override
    {
        entered = true;
        gate.wait();
        throw moonbase::api_error(0, "cancelled");
    }

    void cancel() { gate.signal(); }
};
} // namespace

TEST_CASE("destroying the controller mid-request cancels and joins without hanging")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["validated"] = now_seconds() - 3600; // past the throttle -> start() hits the network
    fx.seedStored(fx.token(claims));

    auto blocking = std::make_shared<blocking_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, blocking);

    {
        ActivationController controller(fx.config, licensing, "dev",
                                        [blocking] { blocking->cancel(); });
        controller.start();
        // Wait until the worker is actually blocked inside the transport, so the
        // destructor below genuinely has an in-flight request to cancel.
        REQUIRE(pumpUntil([&] { return blocking->entered.load(); }));
        // Leaving this scope destroys the controller: it must cancel the request
        // and drain the worker promptly. If cancellation were broken this would
        // block on the 5s drain timeout (and then the pool teardown) instead.
    }

    // Reaching here means the destructor cancelled + drained promptly.
    CHECK(blocking->entered.load());
}

TEST_CASE("the HTTP transport cancels every request in flight, not only the latest")
{
    // A server that takes two connections and never answers them, so each
    // request blocks until it is cancelled or times out (a minute, well past the
    // waits below).
    juce::StreamingSocket listener;
    REQUIRE(listener.createListener(0, "127.0.0.1"));
    std::atomic<int> accepted{ 0 };
    std::vector<std::unique_ptr<juce::StreamingSocket>> connections;
    std::thread server([&]
    {
        while (connections.size() < 2)
        {
            std::unique_ptr<juce::StreamingSocket> connection(listener.waitForNextConnection());
            if (connection == nullptr)
                return;
            connections.push_back(std::move(connection));
            ++accepted;
        }
    });

    moonbase::juce_integration::juce_http_transport transport;
    moonbase::http_request request;
    request.url = "http://127.0.0.1:" + std::to_string(listener.getBoundPort()) + "/";
    request.connect_timeout = std::chrono::seconds(60);
    std::atomic<int> finished{ 0 };
    const auto send = [&]
    {
        try
        {
            (void) transport.send(request);
        }
        catch (const std::exception&)
        {
        }
        ++finished;
    };
    std::thread first(send);
    std::thread second(send);

    const auto waitFor = [](const std::function<bool()>& done, std::chrono::milliseconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (! done() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return done();
    };
    CHECK(waitFor([&] { return accepted.load() == 2; }, std::chrono::seconds(15)));

    // Both are waiting for an answer: one cancel() has to reach the two of them.
    transport.cancel();
    CHECK(waitFor([&] { return finished.load() == 2; }, std::chrono::seconds(5)));

    first.join();
    second.join();
    listener.close();
    server.join();
}

TEST_CASE("destroying the controller while another instance holds the license lock does not wait for it")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["validated"] = now_seconds() - 3600; // past the throttle -> start() checks online, under the lock
    fx.seedStored(fx.token(claims));
    fx.config.deviceIdResolver = fx.fingerprint;

    // Another instance holds the lock for its own online check, which offline
    // can run until its request times out. Released after 3 s at the latest, so
    // a controller that waits for it fails this test instead of hanging it.
    moonbase::file_license_store sibling(compat::toFilesystemPath(fx.licenseFile));
    auto held = sibling.lock_for_update();
    juce::WaitableEvent tornDown;
    std::thread releaser([&held, &tornDown]
    {
        tornDown.wait(3000);
        held.reset();
    });

    // The primary constructor, so the teardown runs exactly as in a plugin.
    auto controller = std::make_unique<ActivationController>(fx.config);
    controller->start();
    pumpFor(300); // the worker is waiting for the lock by now

    const auto before = std::chrono::steady_clock::now();
    controller.reset();
    const auto teardown = std::chrono::steady_clock::now() - before;
    tornDown.signal();
    releaser.join();

    CHECK(teardown < std::chrono::milliseconds(1500));
}

TEST_CASE("the worker pool runs every job it is given")
{
    std::atomic<int> ran{0};
    {
        detail::WorkerPool pool(2);
        for (int i = 0; i < 50; ++i)
            pool.addJob([&ran] { ++ran; });
        REQUIRE(pumpUntil([&] { return ran.load() == 50; }));
    }
    CHECK(ran.load() == 50);
}

TEST_CASE("stopping the worker pool waits for running jobs and drops queued ones")
{
    std::atomic<bool> started{false}, release{false}, finished{false};
    std::atomic<int> queuedRan{0};

    detail::WorkerPool pool(1);
    pool.addJob([&]
    {
        started = true;
        while (! release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        finished = true;
    });
    for (int i = 0; i < 5; ++i)
        pool.addJob([&queuedRan] { ++queuedRan; }); // behind the busy worker
    REQUIRE(pumpUntil([&] { return started.load(); }));

    std::thread releaser([&]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release = true;
    });
    pool.stop();
    releaser.join();

    CHECK(finished.load());       // the running job was waited for, however long it took
    CHECK(queuedRan.load() == 0); // the queued ones never started
}

TEST_CASE("the worker exit hook runs once on every worker")
{
    std::atomic<int> exits{0};
    {
        detail::WorkerPool pool(2);
        pool.setWorkerExitHook([&exits] { ++exits; });
        pool.stop();
        CHECK(exits.load() == 2);
    }
    CHECK(exits.load() == 2); // and not again from the destructor
}

TEST_CASE("worker pools come and go without stalling")
{
    // The juce::ThreadPool this replaces sometimes killed an idle thread here
    // on JUCE 6.1.3, and a later teardown could hang.
    juce::Random random(7);
    const auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i)
    {
        detail::WorkerPool pool(2);
        pool.addJob([] {});
        std::this_thread::sleep_for(std::chrono::milliseconds(random.nextInt(20)));
    }
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(30));
}

//==============================================================================
// Device identity
//==============================================================================
TEST_CASE("the module default is the cross-SDK spec resolver, except on iOS")
{
    controller_fixture fx;

    auto resolved = fx.config.resolvedDeviceIdResolver();
    REQUIRE(resolved != nullptr);

    if (ActivationConfig::hasScopedIdentityOnly)
    {
        // iOS and Android get a scoped spec identity, because their only device
        // identifiers are scoped by the platform (identifierForVendor to the
        // vendor, ANDROID_ID to the app signing key). The mbd2s_ stamp makes that
        // legible rather than implicit. Deliberately NOT the host-name fallback:
        // since iOS 17 gethostname() returns "localhost" on every device and
        // UIDevice.name returns "iPhone", so it would give a whole install base
        // one id.
        const auto id = resolved->device_id();
        CHECK(id.rfind("mbd2s_", 0) == 0);
        CHECK(id.size() == 70);

        const auto stamp = moonbase::fingerprint_spec::parse_device_id_stamp(id);
        REQUIRE(stamp.has_value());
        CHECK(stamp->source == moonbase::fingerprint_spec::device_id_source::scoped);

        const auto described = resolved->describe_device();
        REQUIRE(described.has_value());
        CHECK(described->platform == (ActivationConfig::isAndroid ? "android" : "ios"));
        CHECK(described->param_names
            == std::vector<std::string>{
                ActivationConfig::isAndroid ? "androidId" : "identifierForVendor"});

        // And it came from the core resolver, not a JUCE-specific one: the SDK
        // reads every platform natively, which is what lets the bridge and any
        // non-JUCE consumer get the same id.
        CHECK(described->version == moonbase::fingerprint_spec::version);
        return;
    }

    // Everywhere else: an unconfigured plugin computes the same device id as
    // @moonbase.sh/licensing on the same machine, which is the point of 4.0.0.
    try
    {
        const auto id = resolved->device_id();
        CHECK(id.rfind("mbd2_", 0) == 0);
        CHECK(id.size() == 69);
    }
    catch (const moonbase::insufficient_device_identity_error&)
    {
        // A runner may genuinely have no hardware identity, and refusing is the
        // correct answer. What matters is that the default is no longer the old
        // SystemStats id, which never throws and never carries a stamp.
        MESSAGE("no hardware identity on this host");
    }
}

TEST_CASE("allowDeviceNameFallback opts into the weaker host-name id")
{
    controller_fixture fx;
    fx.config.allowDeviceNameFallback = true;

    if (ActivationConfig::hasScopedIdentityOnly)
    {
        // Forbidden outright on iOS and Android: the host name there is
        // "localhost" or a model name, so the fallback would hand an entire
        // install base one device id. The ladder is scoped, then insufficient.
        const auto id = fx.config.resolvedDeviceIdResolver()->device_id();
        CHECK(id.rfind("mbd2s_", 0) == 0);
        return;
    }

    const auto described = fx.config.resolvedDeviceIdResolver()->describe_device();
    REQUIRE(described.has_value());

    // Whether this host has hardware identity or not, the weaker binding must be
    // separately stamped so the server and support can tell them apart.
    const bool stamped_weaker = described->device_id.rfind("mbd2n_", 0) == 0;
    CHECK((described->source == moonbase::fingerprint_spec::device_id_source::device_name)
        == stamped_weaker);
}

TEST_CASE("an explicit deviceIdResolver overrides the default")
{
    controller_fixture fx;
    fx.config.deviceIdResolver =
        std::make_shared<moonbase::static_device_id_resolver>("Studio Mac", "custom-device-id");

    CHECK(fx.config.resolvedDeviceIdResolver()->device_id() == "custom-device-id");

    // A custom resolver's id is compared literally and needs no mbd2_ stamp.
    // seedRawToken, not seedStored: the fixture's own resolver would reject a token
    // bound to this config's id before it could be written.
    fx.seedRawToken(fx.token(default_claims("custom-device-id")));
    ActivationController controller(fx.config);
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));
    CHECK(controller.screen() == Screen::Details);
}

TEST_CASE("a migrating resolver keeps a license bound under the old id working")
{
    controller_fixture fx;

    // What an already-shipped plugin does on upgrade: bind the spec id on new
    // activations, while still accepting the id this device was bound to before.
    const std::string legacyId = "old-juce-unique-device-id";
    fx.config.deviceIdResolver = std::make_shared<moonbase::migrating_device_id_resolver>(
        std::make_shared<moonbase::static_device_id_resolver>("Studio Mac", "mbd2_" + std::string(64, 'a')),
        std::make_shared<moonbase::static_device_id_resolver>("Studio Mac", legacyId));

    // seedRawToken: the fixture's resolver is not the migrating one under test.
    fx.seedRawToken(fx.token(default_claims(legacyId)));

    ActivationController controller(fx.config);
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));

    // Without the wrapper this lands on Welcome with the user locked out, costing
    // them a re-activation and an activation seat.
    CHECK(controller.screen() == Screen::Details);
}

TEST_CASE("without a migration, an old binding is diagnosed as a device mismatch")
{
    controller_fixture fx;
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& message) { diags.add(message); };
    fx.config.deviceIdResolver =
        std::make_shared<moonbase::static_device_id_resolver>("Studio Mac", "mbd2_" + std::string(64, 'a'));

    fx.seedRawToken(fx.token(default_claims("old-juce-unique-device-id")));

    ActivationController controller(fx.config);
    controller.start();
    REQUIRE(pumpUntil([&] { return settled(controller); }));

    // The diagnostic must name the real problem and point at the remedy, rather
    // than the old blanket "not valid for this device".
    const auto joined = diags.joinIntoString(" | ");
    INFO("diagnostics: " << joined);
    CHECK(joined.contains("not bound to this device"));
    CHECK(joined.contains("migrating_device_id_resolver"));
}

TEST_CASE("describeDevice reports provenance, and nothing for an opaque resolver")
{
    controller_fixture fx;
    ActivationController controller(fx.config);

    if (const auto described = controller.describeDevice())
    {
        CHECK(described->version == 2);
        CHECK(!described->platform.empty());
        for (const auto& name : described->param_names)
            CHECK(!name.empty());
    }

    SUBCASE("a custom resolver that cannot describe itself yields nothing")
    {
        controller_fixture custom;
        custom.config.deviceIdResolver =
            std::make_shared<moonbase::static_device_id_resolver>("Studio Mac", "custom-device-id");
        ActivationController other(custom.config);
        CHECK(!other.describeDevice().has_value());
    }
}

//==============================================================================
// Theming: ActivationConfig carries the palette + typefaces the UI paints
// through, so a re-skin is in place before the component builds its icons.

TEST_CASE("the look and feel resolves the palette it is given")
{
    ActivationPalette custom;
    custom.backgroundTop = juce::Colour(0xff1a1512);
    custom.cardFill = juce::Colour(0x0affe8d0);
    custom.onAccent = juce::Colour(0xff2b1d10);

    ActivationLookAndFeel lnf(juce::Colour(0xffe4a03c), custom);

    CHECK(lnf.accent == juce::Colour(0xffe4a03c));
    CHECK(lnf.palette.backgroundTop == custom.backgroundTop);
    CHECK(lnf.palette.cardFill == custom.cardFill);
    CHECK(lnf.palette.onAccent == custom.onAccent);
    // Untouched tokens keep the design's defaults.
    CHECK(lnf.palette.textPrimary == ActivationPalette{}.textPrimary);
    // The window background tracks the palette, not the built-in near-black.
    CHECK(lnf.findColour(juce::ResizableWindow::backgroundColourId)
          == custom.backgroundBottom);
}

TEST_CASE("a config with no theme keeps the built-in design")
{
    ActivationConfig config;
    ActivationLookAndFeel lnf(config.accent, config.palette, config.fonts);

    CHECK(lnf.palette.backgroundTop == ActivationPalette{}.backgroundTop);
    CHECK(lnf.heading(14.0f).getHeight() == doctest::Approx(14.0f));
    CHECK(lnf.heading(14.0f).isBold());
    CHECK(!lnf.body(14.0f).isBold());
}

TEST_CASE("fonts.makeFont takes over every font the UI asks for")
{
    using Role = ActivationFonts::Role;

    std::vector<std::pair<Role, float>> asked;
    ActivationFonts fonts;
    fonts.makeFont = [&asked](Role role, float height)
    {
        asked.emplace_back(role, height);
        return compat::font(height).withStyle(juce::Font::italic);
    };

    ActivationLookAndFeel lnf(juce::Colour(0xff186cdc), {}, fonts);

    CHECK(lnf.heading(20.0f).isItalic());
    CHECK(lnf.body(13.0f).isItalic());
    CHECK(lnf.mono(11.0f).isItalic());

    REQUIRE(asked.size() == 3);
    CHECK(asked[0] == std::make_pair(Role::heading, 20.0f));
    CHECK(asked[1] == std::make_pair(Role::body, 13.0f));
    CHECK(asked[2] == std::make_pair(Role::mono, 11.0f));
}

TEST_CASE("a role typeface is used for that role only")
{
    // A tiny valid TTF is more machinery than this needs: a system typeface is
    // enough to prove the plumbing, since the fallback path never sets one.
    auto face = compat::font(12.0f).getTypefacePtr();
    if (face == nullptr)
        return; // no resolvable system font on this box; the fallbacks are covered above

    ActivationFonts fonts;
    fonts.mono = face;

    ActivationLookAndFeel lnf(juce::Colour(0xff186cdc), {}, fonts);

    CHECK(lnf.mono(12.5f).getTypefacePtr() == face);
    CHECK(lnf.mono(12.5f).getHeight() == doctest::Approx(12.5f));
    // The other roles are untouched, so they still resolve at paint time.
    CHECK(lnf.heading(12.5f).isBold());
}

//==============================================================================
// The animation seam has two backends (juce_animation when the project links it,
// the module's own otherwise). These pin the semantics the UI depends on, and run
// against whichever one this build picked.

TEST_CASE("an animation eases from 0 to 1 and completes exactly once")
{
    anim::Updater updater;
    std::vector<float> values;
    int completions = 0;

    auto animation = anim::AnimationBuilder{}
                         .withDurationMs(100.0)
                         .withEasing(anim::easings::createLinear())
                         .withValueChangedCallback([&](float v) { values.push_back(v); })
                         .withOnCompleteCallback([&] { ++completions; })
                         .build();
    updater.addAnimation(animation);
    animation.start();

    // The first tick is the start of the timeline, not an offset into it, so the
    // ticks below are 0/25/50/75/100 percent of the duration.
    for (int i = 0; i <= 6; ++i)
        updater.update(static_cast<double>(i) * 25.0);

    REQUIRE(values.size() >= 5);
    CHECK(values.front() == doctest::Approx(0.0f));
    CHECK(values[2] == doctest::Approx(0.5f));
    // The frame that finishes reports exactly 1.0, and nothing is reported after.
    CHECK(values.back() == doctest::Approx(1.0f));
    CHECK(completions == 1);
}

TEST_CASE("an infinite animation keeps advancing past 1.0")
{
    // This is the panel glow: linear, infinite, wrapped into [0,1) by the caller.
    // Any other easing flattens past 1.0 (the curve extrapolates by its end-point
    // gradient, 0 for the default ease) and the glow would stop after one pass.
    anim::Updater updater;
    std::vector<float> values;

    auto animation = anim::AnimationBuilder{}
                         .withDurationMs(100.0)
                         .runningInfinitely()
                         .withEasing(anim::easings::createLinear())
                         .withValueChangedCallback([&](float v) { values.push_back(v); })
                         .build();
    updater.addAnimation(animation);
    animation.start();

    for (int i = 0; i <= 25; ++i)
        updater.update(static_cast<double>(i) * 10.0);

    REQUIRE(values.size() >= 26);
    CHECK(values.back() == doctest::Approx(2.5f));           // 250ms of a 100ms loop
    CHECK(std::fmod(values.back(), 1.0f) == doctest::Approx(0.5f));
}

TEST_CASE("the shipped easing curves match the ones juce::Easings builds")
{
    // Same cubic-bezier control points either way, so a build with juce_animation
    // and a build without it animate identically.
    const auto easeOut = anim::easings::createEaseOut();
    const auto easeOutBack = anim::easings::createEaseOutBack();
    const auto easeInOutCubic = anim::easings::createEaseInOutCubic();

    for (const auto& easing : { easeOut, easeOutBack, easeInOutCubic })
    {
        CHECK(easing(0.0f) == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(easing(1.0f) == doctest::Approx(1.0f).epsilon(0.001));
    }

    // cubic-bezier(0, 0, 0.58, 1): decelerating, so it is ahead of linear.
    CHECK(easeOut(0.5f) > 0.5f);
    // cubic-bezier(0.65, 0, 0.35, 1): symmetric about the midpoint.
    CHECK(easeInOutCubic(0.5f) == doctest::Approx(0.5f).epsilon(0.001));
    // cubic-bezier(0.34, 1.56, 0.64, 1): overshoots 1.0 before settling.
    CHECK(easeOutBack(0.6f) > 1.0f);
}

//==============================================================================
// Add-ons
//==============================================================================
namespace {

// Takes a list rather than a json, which a single braced entry would initialise
// as that entry instead of a list of one.
nlohmann::json withAddOns(std::initializer_list<nlohmann::json> addOns)
{
    auto claims = default_claims();
    claims["sp"] = nlohmann::json::array();
    for (const auto& entry : addOns)
        claims["sp"].push_back(entry);
    return claims;
}

nlohmann::json addOn(const char* id, bool active, bool trial = false)
{
    return { { "id", id }, { "active", active }, { "trial", trial } };
}

const char* const addOnList = R"({
    "currency": "EUR",
    "subProducts": [
        {"id": "strings", "product": {"id": "demo-app.strings", "name": "Strings", "tagline": "Bowed and plucked", "owned": false, "subscribed": false,
            "defaultVariation": {"id": "one-off", "name": "Strings", "originalPrice": {"EUR": 49}, "price": {"EUR": 39}, "hasDiscount": true, "recurrence": {"type": "OneOff"}}},
         "includedInLicense": true, "active": false, "trial": {"status": "Available", "numberOfDays": 7, "requiresNewsletterOptIn": false},
         "buyUrl": "https://demo.moonbase.sh/buy/demo-app.strings"},
        {"id": "pack", "product": {"id": "demo-app.pack", "name": "Pack", "tagline": "", "owned": true, "subscribed": false},
         "includedInLicense": true, "active": true, "seats": {"total": 2, "used": 1, "remaining": 1}}
    ]
})";

moonbase::http_response problemResponse(const char* errorType, const char* detail)
{
    nlohmann::json body{ { "title", "Refused" }, { "detail", detail }, { "status", 400 } };
    if (errorType != nullptr)
        body["errorType"] = errorType;
    return moonbase::http_response{ 400, {}, body.dump() };
}

// The add-on list with one add-on, drum-kit, included in the license.
moonbase::http_response drumKitList(bool owned, bool active, bool forSale = false)
{
    nlohmann::json entry{
        { "id", "drum-kit" },
        { "product", { { "id", "demo-app.drum-kit" }, { "name", "Drum Kit" }, { "tagline", "" },
                       { "owned", owned }, { "subscribed", false } } },
        { "includedInLicense", true },
        { "active", active },
    };
    if (owned)
        entry["seats"] = { { "total", 2 }, { "used", active ? 1 : 0 }, { "remaining", active ? 1 : 2 } };
    if (forSale)
        entry["buyUrl"] = "https://demo.moonbase.sh/buy/demo-app.drum-kit";
    nlohmann::json list{ { "currency", "EUR" }, { "subProducts", nlohmann::json::array({ entry }) } };
    return moonbase::http_response{ 200, {}, list.dump() };
}


} // namespace

TEST_CASE("hasSubProduct reads the add-ons the license includes")
{
    controller_fixture fx;
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true), addOn("brass", false), addOn("strings", true, true) })));
    ActivationController controller(fx.config, fx.makeLicensing());
    CHECK_FALSE(controller.hasSubProduct("pack")); // nothing loaded yet
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    CHECK(controller.hasSubProduct("pack"));
    CHECK(controller.hasSubProduct("demo-app.strings"));
    CHECK_FALSE(controller.hasSubProduct("brass"));
    CHECK_FALSE(controller.hasSubProduct("keys"));
    CHECK(controller.activeSubProducts() == juce::StringArray{ "pack", "strings" });

    controller.clearLicense();
    CHECK_FALSE(controller.hasSubProduct("pack"));
    CHECK(controller.activeSubProducts().isEmpty());
}

TEST_CASE("subProductFlag follows hasSubProduct for the audio thread, in step with licensedFlag")
{
    controller_fixture fx;
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true), addOn("brass", false) })));
    ActivationController controller(fx.config, fx.makeLicensing());
    // Asked for before anything is loaded, as a processor's constructor would.
    const auto* pack = controller.subProductFlag("pack");
    const auto* brass = controller.subProductFlag("brass");
    REQUIRE(pack != nullptr);
    REQUIRE(brass != nullptr);
    CHECK(controller.subProductFlag("pack") == pack); // one flag per ID
    CHECK_FALSE(pack->load());

    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    CHECK(pack->load());
    CHECK_FALSE(brass->load());
    CHECK(controller.subProductFlag("demo-app.pack")->load()); // the whole ID, asked for late: current from the start
    CHECK_FALSE(controller.subProductFlag("keys")->load());    // one the license doesn't include

    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ addOn("pack", true), addOn("brass", true) })) });
    bool done = false;
    controller.activateSubProduct("brass", [&](bool, const juce::String&) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(brass->load());

    // Down in the same call as licensedFlag, before onLicenseChanged is delivered.
    controller.clearLicense();
    CHECK_FALSE(controller.licensedFlag().load());
    CHECK_FALSE(pack->load());
    CHECK_FALSE(brass->load());
}

TEST_CASE("an add-on trial that runs out while the plugin is open is reported, and the license stays")
{
    controller_fixture fx;
    auto strings = addOn("strings", true, true);
    strings["exp"] = now_seconds() + 2;
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true), strings })));
    ActivationController controller(fx.config, fx.makeLicensing());
    std::vector<bool> reports;
    controller.onLicenseChanged = [&](bool licensed) { reports.push_back(licensed); };
    const auto* stringsFlag = controller.subProductFlag("strings");
    controller.start();
    REQUIRE(pumpUntil([&] { return reports.size() == 1; }));
    CHECK(controller.hasSubProduct("strings"));
    CHECK(stringsFlag->load());

    // No new token: the license watch notices the add-on's end on its own, and
    // re-checks the license once in case it was bought meanwhile. This one
    // wasn't (the check gets no answer here), so it stays locked.
    REQUIRE(pumpUntil([&] { return reports.size() == 2; }, 8000));
    CHECK(reports.back());
    CHECK_FALSE(controller.hasSubProduct("strings"));
    CHECK_FALSE(stringsFlag->load());
    CHECK(controller.hasSubProduct("pack"));
    CHECK(controller.licensedFlag().load());
    CHECK(controller.screen() == Screen::Details);
    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 1; }));

    pumpFor(2500);
    CHECK(reports.size() == 2);                  // reported once
    CHECK(fx.transport->requests.size() == 1);   // and re-checked once
}

TEST_CASE("an add-on bought during its trial stays unlocked when the trial ends")
{
    controller_fixture fx;
    auto drumKit = addOn("drum-kit", true, true);
    drumKit["exp"] = now_seconds() + 2;
    fx.seedStored(fx.token(withAddOns({ drumKit })));
    ActivationController controller(fx.config, fx.makeLicensing());
    std::vector<bool> reports;
    controller.onLicenseChanged = [&](bool licensed) { reports.push_back(licensed); };
    controller.start();
    REQUIRE(pumpUntil([&] { return reports.size() == 1; }));
    REQUIRE(controller.hasSubProduct("drum-kit"));

    // Bought on the web meanwhile: when the trial runs out, the re-check takes the seat.
    auto bought = withAddOns({ addOn("drum-kit", true) });
    bought["validated"] = now_seconds();
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(bought) });

    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 1; }, 8000));
    REQUIRE(pumpUntil([&] { return controller.hasSubProduct("drum-kit")
                                   && ! moonbase::find_sub_product(*controller.license(), "drum-kit")->trial; }));
    CHECK(fx.transport->requests[0].url.find("/validate?") != std::string::npos);
}

TEST_CASE("activateSubProduct takes a seat, then loads and stores the license that comes back")
{
    controller_fixture fx;
    fx.seedStored(fx.token(withAddOns({ addOn("strings", false) })));
    ActivationController controller(fx.config, fx.makeLicensing());
    std::vector<bool> reports;
    controller.onLicenseChanged = [&](bool licensed) { reports.push_back(licensed); };
    controller.start();
    REQUIRE(pumpUntil([&] { return reports.size() == 1; }));
    CHECK_FALSE(controller.hasSubProduct("strings"));

    const auto fresh = fx.token(withAddOns({ addOn("strings", true) }));
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fresh });
    bool done = false, ok = false;
    controller.activateSubProduct("strings", [&](bool activated, const juce::String&) { done = true; ok = activated; });
    CHECK(controller.addOns().busyId == "strings");

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(ok);
    CHECK(controller.addOns().busyId.isEmpty());
    CHECK(controller.hasSubProduct("strings"));
    REQUIRE(fx.transport->requests.size() == 1);
    CHECK(fx.transport->requests.front().url.find("/api/client/licenses/demo-app/sub-products/strings/activate?")
          != std::string::npos);
    const auto stored = fx.store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == fresh);
    REQUIRE(pumpUntil([&] { return reports.size() == 2; }));
    CHECK(reports.back());
    CHECK(controller.screen() == Screen::Details); // nothing moved
}

TEST_CASE("requestSubProductTrial starts the add-on's trial from the license")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    auto strings = addOn("strings", true, true);
    strings["exp"] = now_seconds() + 7 * 86400;
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ strings })) });
    bool done = false, ok = false;
    controller.requestSubProductTrial("strings", [&](bool started, const juce::String&) { done = true; ok = started; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(ok);
    CHECK(controller.hasSubProduct("strings"));
    CHECK(fx.transport->requests.front().url.find("/sub-products/strings/trial?") != std::string::npos);
}

TEST_CASE("an add-on with no seat to give says so, and the license stays")
{
    controller_fixture fx;
    juce::StringArray diags;
    fx.config.onDiagnostic = [&](const juce::String& m) { diags.add(m); };
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(problemResponse("NoEligibleLicense", "No license with available activations found"));
    bool done = false, ok = true;
    juce::String message;
    controller.activateSubProduct("strings", [&](bool activated, const juce::String& m) { done = true; ok = activated; message = m; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);
    CHECK(message.startsWith("No seat of this add-on is free"));
    CHECK(message.contains("Helio Audio account"));
    CHECK(controller.addOns().actionError == message);
    CHECK(diags.joinIntoString(" ").contains("No license with available activations found"));
    CHECK(controller.licensedFlag().load());
    CHECK(controller.screen() == Screen::Details);
    CHECK(fx.transport->requests.size() == 1); // no re-check: the license was never in question
}

TEST_CASE("an add-on refused for this license says so, and the license stays")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(problemResponse(nullptr, "A sub-product trial can only be started from an online activation"));
    bool done = false;
    juce::String message;
    controller.requestSubProductTrial("strings", [&](bool, const juce::String& m) { done = true; message = m; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(message == "This add-on can't be tried on this computer.");
    CHECK(controller.licensedFlag().load());
}

TEST_CASE("an add-on call the server refuses for the license re-checks it, and locks")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    const auto revoked = problemResponse("LicenseActivationRevoked", "License has been revoked");
    fx.transport->responses.push_back(revoked); // the add-on call
    fx.transport->responses.push_back(revoked); // the re-check it leads to
    bool done = false, ok = true;
    controller.activateSubProduct("strings", [&](bool activated, const juce::String&) { done = true; ok = activated; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);
    REQUIRE(pumpUntil([&] { return ! controller.license().has_value(); }));
    CHECK(controller.screen() == Screen::Welcome);
    CHECK(controller.lockReason() == LockReason::Invalid);
    CHECK(fx.transport->requests.size() == 2);
}

TEST_CASE("add-ons are not activated or tried from a trial")
{
    controller_fixture fx;
    auto claims = default_claims();
    claims["trial"] = true;
    fx.seedStored(fx.token(claims));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Trial; }));

    bool done = false, ok = true;
    controller.activateSubProduct("strings", [&](bool activated, const juce::String&) { done = true; ok = activated; });
    CHECK(done); // refused right away
    CHECK_FALSE(ok);

    controller.showAddOns();
    CHECK(controller.screen() == Screen::Trial);
    pumpFor(100);
    CHECK(fx.transport->requests.empty());
}

TEST_CASE("one add-on call at a time")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ addOn("strings", true) })) });
    }
    bool firstDone = false, firstOk = false;
    controller.activateSubProduct("strings", [&](bool ok, const juce::String&) { firstDone = true; firstOk = ok; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));

    bool secondDone = false, secondOk = true;
    controller.requestSubProductTrial("keys", [&](bool ok, const juce::String&) { secondDone = true; secondOk = ok; });
    CHECK(secondDone);
    CHECK_FALSE(secondOk);
    CHECK(controller.addOns().busyId == "strings");

    gated->release();
    REQUIRE(pumpUntil([&] { return firstDone; }));
    CHECK(firstOk);
    CHECK(gated->requests.load() == 1);
}

TEST_CASE("an add-on that lands after the license is cleared does not bring it back")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ addOn("strings", true) })) });
    }
    bool done = false, ok = true;
    controller.activateSubProduct("strings", [&](bool activated, const juce::String&) { done = true; ok = activated; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));

    // Clearing waits for the request under way, which holds the store's lock
    // until it is answered (so the answer is stored first, and deleted with the
    // rest): let it be answered.
    std::thread answer([gated] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); gated->release(); });
    controller.clearLicense();
    answer.join();
    REQUIRE_FALSE(fx.licenseFile.existsAsFile());

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);
    CHECK_FALSE(controller.license().has_value());
    CHECK_FALSE(fx.licenseFile.existsAsFile());
}

TEST_CASE("a forced refresh waits for an add-on call in progress, so it can't undo it")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    const auto withStrings = fx.token(withAddOns({ addOn("strings", true) }));
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 200, {}, withStrings }); // the add-on call
        gated->responses.push_back(moonbase::http_response{ 200, {}, withStrings }); // the refresh after it
    }
    bool activated = false;
    controller.activateSubProduct("strings", [&](bool ok, const juce::String&) { activated = ok; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));

    bool refreshed = false;
    controller.refreshLicense(true, [&](bool ok) { refreshed = ok; });
    pumpFor(300);
    CHECK(gated->requests.load() == 1); // the refresh has not gone out ahead of the seat

    gated->release();
    REQUIRE(pumpUntil([&] { return activated && refreshed; }));
    CHECK(gated->requests.load() == 2);
    CHECK(controller.hasSubProduct("strings"));
}

TEST_CASE("a throttled refresh that lands after an add-on call does not undo it")
{
    controller_fixture fx;
    // Validated 30s ago: a non-forced check answers from the store. The add-on
    // call answers with the same validation time, as the server does.
    const auto validated = now_seconds() - 30;
    auto seeded = default_claims();
    seeded["validated"] = validated;
    fx.seedStored(fx.token(seeded));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    auto withDrumKitClaims = withAddOns({ addOn("drum-kit", true) });
    withDrumKitClaims["validated"] = validated;
    const auto withDrumKit = fx.token(withDrumKitClaims);
    auto checked = withAddOns({ addOn("drum-kit", true) });
    checked["validated"] = now_seconds();
    const auto checkedToken = fx.token(checked);
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 200, {}, withDrumKit });  // the add-on call
        gated->responses.push_back(moonbase::http_response{ 200, {}, checkedToken }); // the check
    }
    bool activated = false, refreshed = false;
    controller.activateSubProduct("drum-kit", [&](bool ok, const juce::String&) { activated = ok; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));

    // Queued behind the add-on call, and asked with the copy from before it. By
    // then the store holds the add-on's copy, validated at the same time: which
    // is current is the server's to say, so the throttled check asks it.
    controller.refreshLicense(false, [&](bool) { refreshed = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gated->release();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // both answers queued, neither landed

    REQUIRE(pumpUntil([&] { return activated && refreshed; }));
    CHECK(controller.hasSubProduct("drum-kit"));
    CHECK(controller.license()->token == checkedToken);
    const auto stored = fx.store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == checkedToken);
    CHECK(gated->requests.load() == 2);
}

TEST_CASE("a forced refresh queued behind an add-on call applies the server's newer answer")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // The refresh reaches the server after the add-on call, and by then the
    // customer has another add-on too: its answer is the newer one.
    auto later = withAddOns({ addOn("drum-kit", true), addOn("keys", true) });
    later["validated"] = now_seconds();
    const auto newer = fx.token(later);
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });
        gated->responses.push_back(moonbase::http_response{ 200, {}, newer });
    }
    bool activated = false, refreshed = false;
    controller.activateSubProduct("drum-kit", [&](bool ok, const juce::String&) { activated = ok; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));
    controller.refreshLicense(true, [&](bool ok) { refreshed = ok; });
    gated->release();

    REQUIRE(pumpUntil([&] { return activated && refreshed; }));
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->token == newer);
    CHECK(controller.hasSubProduct("drum-kit"));
    CHECK(controller.hasSubProduct("keys"));
    const auto stored = fx.store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == newer);
}

TEST_CASE("a forced refresh queued behind an add-on call applies its answer even when it matches the old license")
{
    controller_fixture fx;
    const auto original = fx.token(default_claims());
    fx.seedStored(original);
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // The add-on is revoked between the two calls, so the server answers the
    // refresh with the very license it was asked about.
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });
        gated->responses.push_back(moonbase::http_response{ 200, {}, original });
    }
    bool activated = false, refreshed = false;
    controller.activateSubProduct("drum-kit", [&](bool ok, const juce::String&) { activated = ok; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));
    controller.refreshLicense(true, [&](bool ok) { refreshed = ok; });
    gated->release();

    REQUIRE(pumpUntil([&] { return activated && refreshed; }));
    CHECK(gated->requests.load() == 2);
    REQUIRE(controller.license().has_value());
    CHECK(controller.license()->token == original);
    CHECK_FALSE(controller.hasSubProduct("drum-kit"));
    const auto stored = fx.store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == original);
}

TEST_CASE("a host may destroy the controller from loadAddOns' callback")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto controller = std::make_unique<ActivationController>(fx.config, fx.makeLicensing());
    controller->start();
    REQUIRE(pumpUntil([&] { return controller->screen() == Screen::Details; }));

    // A list with a seat to take, which would go on to re-check the license.
    fx.transport->responses.push_back(drumKitList(/*owned*/ true, /*active*/ false));
    bool done = false;
    controller->loadAddOns([&](bool) { controller.reset(); done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    pumpFor(300);
    CHECK(fx.transport->requests.size() == 1); // nothing went on after it
}

TEST_CASE("an add-on call that fails after the license changed leaves the new state alone")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(problemResponse("LicenseActivationRevoked", "License has been revoked"));
    }
    bool done = false, ok = true;
    juce::String message;
    controller.activateSubProduct("strings", [&](bool activated, const juce::String& m) { done = true; ok = activated; message = m; });
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));

    // Clearing waits for the request under way to be answered (see above).
    std::thread answer([gated] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); gated->release(); });
    controller.clearLicense();
    answer.join();
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);
    CHECK(message == "Your license changed while this was in progress.");
    CHECK(controller.addOns().actionError.isEmpty());
    CHECK(controller.addOns().busyId.isEmpty());
    pumpFor(200);
    CHECK(gated->requests.load() == 1); // no re-check of a license that is gone
    CHECK(controller.lockReason() == LockReason::Deactivated);
}

TEST_CASE("an add-on taken in one instance unlocks in the others")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController first(fx.config, fx.makeLicensing());
    ActivationController second(fx.config, fx.makeLicensing());
    std::vector<bool> secondReports;
    second.onLicenseChanged = [&](bool licensed) { secondReports.push_back(licensed); };
    const auto* secondDrumKit = second.subProductFlag("drum-kit");
    first.start();
    second.start();
    REQUIRE(pumpUntil([&] { return first.screen() == Screen::Details && second.screen() == Screen::Details
                                   && secondReports.size() == 1; }));

    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });
    bool done = false;
    first.activateSubProduct("drum-kit", [&](bool, const juce::String&) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(first.hasSubProduct("drum-kit"));

    // Picked up from the license file the first instance wrote, with no request.
    REQUIRE(pumpUntil([&] { return second.hasSubProduct("drum-kit"); }, 15000)); // a 2 s watch, late on a busy machine
    CHECK(secondDrumKit->load());
    REQUIRE(pumpUntil([&] { return secondReports.size() == 2; }));
    CHECK(secondReports.back());
    CHECK(second.screen() == Screen::Details);
    CHECK(fx.transport->requests.size() == 1);
}


TEST_CASE("an add-on bought since the license was checked takes its seat when the add-ons are listed")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(drumKitList(/*owned*/ true, /*active*/ false));        // the list
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });                     // the re-check takes the seat
    fx.transport->responses.push_back(drumKitList(/*owned*/ true, /*active*/ true));         // listed again
    controller.showAddOns();

    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 3
                                   && controller.addOns().phase == ActivationController::AddOnsInfo::Phase::Ready; }));
    CHECK(controller.hasSubProduct("drum-kit"));
    CHECK(fx.transport->requests[1].url.find("/validate?") != std::string::npos);
    CHECK(controller.screen() == Screen::AddOns);
    CHECK(controller.addOns().addOns.at(0).active);
}

TEST_CASE("listing an add-on bought during its trial takes its seat")
{
    controller_fixture fx;
    auto drumKit = addOn("drum-kit", true, true);
    drumKit["exp"] = now_seconds() + 86400;
    fx.seedStored(fx.token(withAddOns({ drumKit })));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(drumKitList(/*owned*/ true, /*active*/ true)); // on its trial, and owned
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });               // the re-check: a seat now
    fx.transport->responses.push_back(drumKitList(true, true));
    controller.showAddOns();

    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 3
                                   && controller.addOns().phase == ActivationController::AddOnsInfo::Phase::Ready; }));
    const auto* held = moonbase::find_sub_product(*controller.license(), "drum-kit");
    REQUIRE(held != nullptr);
    CHECK_FALSE(held->trial);
}

TEST_CASE("an add-on on this device's last seat that the license doesn't show is picked up")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    // The list says this device holds the only seat (an answer that never reached
    // the license here), so none is free, but the check brings it back.
    nlohmann::json list = nlohmann::json::parse(drumKitList(true, true).body);
    list["subProducts"][0]["seats"] = { { "total", 1 }, { "used", 1 }, { "remaining", 0 } };
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, list.dump() });
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, list.dump() });
    controller.showAddOns();

    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 3; }));
    REQUIRE(pumpUntil([&] { return controller.hasSubProduct("drum-kit"); }));
}

TEST_CASE("an add-on the server still won't seat is re-checked once, not over and over")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(drumKitList(true, false));
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(default_claims()) });
    fx.transport->responses.push_back(drumKitList(true, false));
    controller.showAddOns();

    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 3; }));
    pumpFor(500);
    CHECK(fx.transport->requests.size() == 3);
    CHECK_FALSE(controller.hasSubProduct("drum-kit")); // "Activate" is the way now
}

TEST_CASE("with autoActivateSubProducts off, listing add-ons never takes a seat")
{
    controller_fixture fx;
    fx.config.autoActivateSubProducts = false;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(drumKitList(true, false));
    bool done = false;
    controller.loadAddOns([&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    pumpFor(300);
    CHECK(fx.transport->requests.size() == 1);
}

TEST_CASE("coming back from buying an add-on in the browser refreshes the add-ons")
{
    controller_fixture fx;
    bool inForeground = true;
    int looks = 0; // how often the controller asked whether it is in front
    fx.config.openBrowser = [](const juce::URL&) { return true; };
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.setForegroundCheck([&] { ++looks; return inForeground; });
    // Until the license watch (every 2 s, later on a busy machine) has looked again.
    const auto untilLookedAgain = [&]
    {
        const int before = looks;
        return pumpUntil([&] { return looks > before; }, 15000);
    };
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(drumKitList(/*owned*/ false, /*active*/ false, /*forSale*/ true));
    bool listed = false;
    controller.loadAddOns([&](bool) { listed = true; });
    REQUIRE(pumpUntil([&] { return listed; }));

    REQUIRE(controller.buySubProduct("drum-kit"));
    REQUIRE(untilLookedAgain()); // still in front: the browser hasn't taken over yet, so nothing happens
    pumpFor(100);
    CHECK(fx.transport->requests.size() == 1);

    inForeground = false; // the customer is buying in the browser
    REQUIRE(untilLookedAgain());
    pumpFor(100);
    CHECK(fx.transport->requests.size() == 1);

    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, fx.token(withAddOns({ addOn("drum-kit", true) })) });
    fx.transport->responses.push_back(drumKitList(/*owned*/ true, /*active*/ true));
    inForeground = true; // and back
    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 3
                                   && controller.addOns().phase == ActivationController::AddOnsInfo::Phase::Ready; },
                      15000));
    CHECK(controller.hasSubProduct("drum-kit"));
    CHECK(fx.transport->requests[1].url.find("/validate?") != std::string::npos);

    // Once: it no longer watches, so going to the background and back again
    // later is not taken for another purchase.
    const int looked = looks;
    inForeground = false;
    pumpFor(2500);
    inForeground = true;
    pumpFor(2500);
    CHECK(looks == looked);
    CHECK(fx.transport->requests.size() == 3);
}

TEST_CASE("a refresh asked for during a deactivation is skipped, and the deactivation finishes")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    auto gated = std::make_shared<gated_transport>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), fx.store, fx.fingerprint, gated);
    ActivationController controller(fx.config, licensing, "dev", [gated] { gated->release(); });
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->responses.push_back(moonbase::http_response{ 503, {}, R"({"message":"Service Unavailable"})" });
    }
    controller.deactivate();
    REQUIRE(pumpUntil([&] { return gated->entered.load(); }));
    REQUIRE(controller.isBusy());

    // The add-ons' "Refresh", a purchase coming back, or the host.
    bool refreshed = true;
    controller.refreshLicense(true, [&](bool ok) { refreshed = ok; });
    CHECK_FALSE(refreshed); // turned down at once
    controller.refreshAddOns();

    gated->release(); // the revoke fails: the license is kept
    REQUIRE(pumpUntil([&] { return ! controller.isBusy(); }));
    CHECK(controller.screen() == Screen::Details);
    CHECK(controller.statusMessage().isNotEmpty());
    CHECK(controller.licensedFlag().load());
    pumpFor(200);
    CHECK(gated->requests.load() == 1);
}

namespace {

// A store whose writes fail while failWrites is set, as on a full disk.
struct flaky_store : moonbase::memory_license_store
{
    std::atomic<bool> failWrites{ false };

    void store_local_license(const moonbase::license& value) override
    {
        if (failWrites.load())
            throw moonbase::storage_error("disk full");
        moonbase::memory_license_store::store_local_license(value);
    }
};

} // namespace

TEST_CASE("a license whose first save failed is saved by the next refresh")
{
    controller_fixture fx;
    fx.config.openBrowser = [](const juce::URL&) { return true; };
    auto store = std::make_shared<flaky_store>();
    auto licensing = std::make_shared<moonbase::licensing>(
        fx.config.toLicensingOptions(), store, fx.fingerprint, fx.transport);
    ActivationController controller(fx.config, licensing);
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Welcome; }));

    store->failWrites = true;
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"});
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(default_claims()) });
    controller.beginOnlineActivation();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Success; }, 8000));
    REQUIRE_FALSE(store->load_local_license().has_value()); // the save failed

    store->failWrites = false;
    auto later = default_claims();
    later["validated"] = now_seconds();
    const auto refreshedToken = fx.token(later);
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, refreshedToken });
    bool done = false;
    controller.refreshLicense(true, [&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));

    const auto stored = store->load_local_license();
    REQUIRE(stored.has_value());
    CHECK(stored->token == refreshedToken);
}

TEST_CASE("a new activation drops the add-ons listed for the one it replaces")
{
    controller_fixture fx;
    fx.config.openBrowser = [](const juce::URL&) { return true; };
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true) })));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, addOnList });
    bool listed = false;
    controller.loadAddOns([&](bool) { listed = true; });
    REQUIRE(pumpUntil([&] { return listed; }));
    REQUIRE_FALSE(controller.addOns().addOns.empty());

    // Activated again here, as another activation.
    auto other = default_claims();
    other["id"] = "activation-456";
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, R"({"id":"request-123","request":"https://demo.moonbase.sh/api/client/activations/request-123?format=JWT","browser":"https://demo.moonbase.sh/activate?token=request-123"})"});
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, fx.token(other) });
    controller.beginOnlineActivation();
    REQUIRE(pumpUntil([&] { return controller.license() && controller.license()->activation_id == "activation-456"; }, 8000));

    CHECK(controller.addOns().addOns.empty());
    CHECK(controller.addOns().phase == ActivationController::AddOnsInfo::Phase::Idle);
}

TEST_CASE("loadAddOns lists the add-ons, and losing the license forgets them")
{
    controller_fixture fx;
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true) }))); // as the list has it
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    using Phase = ActivationController::AddOnsInfo::Phase;
    CHECK(controller.addOns().phase == Phase::Idle);

    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, addOnList });
    bool done = false, ok = false;
    controller.loadAddOns([&](bool loaded) { done = true; ok = loaded; });
    CHECK(controller.addOns().phase == Phase::Loading);

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(ok);
    const auto& info = controller.addOns();
    CHECK(info.phase == Phase::Ready);
    CHECK(info.currency == "EUR");
    REQUIRE(info.addOns.size() == 2);
    CHECK(info.addOns[0].product.name == "Strings");
    CHECK(info.error.isEmpty());
    CHECK(fx.transport->requests.front().url == "https://demo.moonbase.sh/api/client/licenses/demo-app/sub-products");

    controller.clearLicense();
    CHECK(controller.addOns().phase == Phase::Idle);
    CHECK(controller.addOns().addOns.empty());
}

TEST_CASE("an add-on list that fails says why, and the license stays")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(moonbase::http_response{ 503, {}, R"({"message":"Service Unavailable"})" });
    bool done = false, ok = true;
    controller.loadAddOns([&](bool loaded) { done = true; ok = loaded; });

    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(ok);
    CHECK(controller.addOns().error == "Moonbase couldn't do that right now. Try again in a minute.");
    CHECK(controller.licensedFlag().load());
}

TEST_CASE("the add-ons screen stays open through a refresh of its license")
{
    controller_fixture fx;
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true) }))); // as the list has it
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, addOnList });
    controller.showAddOns();
    CHECK(controller.screen() == Screen::AddOns);
    REQUIRE(pumpUntil([&] { return controller.addOns().phase == ActivationController::AddOnsInfo::Phase::Ready; }));

    // "Bought an add-on? Refresh": the license comes back with it, then the list.
    fx.transport->responses.push_back(moonbase::http_response{
        200, {}, fx.token(withAddOns({ addOn("pack", true), addOn("strings", true) })) });
    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, addOnList });
    controller.refreshAddOns();
    REQUIRE(pumpUntil([&] { return fx.transport->requests.size() == 3
                                   && controller.addOns().phase == ActivationController::AddOnsInfo::Phase::Ready; }));
    CHECK(controller.screen() == Screen::AddOns);
    CHECK(controller.hasSubProduct("strings"));
    CHECK(fx.transport->requests[1].url.find("/validate?") != std::string::npos);

    controller.showDetails();
    CHECK(controller.screen() == Screen::Details);
}

namespace
{
// Whether the shown screen (visible all the way down) has an "Activate" button.
bool offersActivate(juce::Component& root)
{
    for (auto* child : root.getChildren())
    {
        if (! child->isVisible())
            continue;
        if (auto* button = dynamic_cast<juce::Button*>(child); button != nullptr && button->getButtonText() == "Activate")
            return true;
        if (offersActivate(*child))
            return true;
    }
    return false;
}
} // namespace

TEST_CASE("the add-ons screen goes by the license for a seat here, and offers Activate while a seat may be free")
{
    controller_fixture fx;
    fx.seedStored(fx.token(default_claims()));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    ActivationComponent component(controller);
    component.setSize(560, 640);

    // Listed while this computer held the add-on's only seat; the license has
    // been refreshed since.
    const auto show = [&](moonbase::sub_product held, moonbase::sub_product_seats listed)
    {
        auto license = *controller.license();
        license.sub_products = { held };
        moonbase::sub_product_catalog_entry entry;
        entry.id = "drum-kit";
        entry.product.id = "demo-app.drum-kit";
        entry.product.name = "Drum Kit";
        entry.product.owned = true;
        entry.included_in_license = true;
        entry.active = true;
        entry.seats = listed;
        ActivationController::AddOnsInfo info;
        info.phase = ActivationController::AddOnsInfo::Phase::Ready;
        info.addOns = { entry };
        controller.setPreviewAddOns(std::move(license), std::move(info));
        pumpFor(50);
        REQUIRE(controller.screen() == Screen::AddOns);
        return offersActivate(component);
    };

    // Revoked here since: the seat is free again, whatever the list counted.
    CHECK(show({ "drum-kit", false, false, true, std::nullopt, moonbase::sub_product_seats{ 1, 0, 1 } },
               moonbase::sub_product_seats{ 1, 1, 0 }));
    // Not revoked, and the license knows of a free seat the list didn't.
    CHECK(show({ "drum-kit", false, false, false, std::nullopt, moonbase::sub_product_seats{ 2, 1, 1 } },
               moonbase::sub_product_seats{ 2, 2, 0 }));
    // Neither knows of one: its seats are in use elsewhere.
    CHECK_FALSE(show({ "drum-kit", false, false, false, std::nullopt, moonbase::sub_product_seats{ 2, 2, 0 } },
                     moonbase::sub_product_seats{ 2, 2, 0 }));
}

TEST_CASE("a change is broadcast when a countdown the screens show passes a day")
{
    controller_fixture fx;
    // An add-on's trial ends a day and 8 seconds from now: "2 days left" at the
    // start, "1 day left" 8 seconds in, with no new license or list in between.
    const auto ends = now_seconds() + 86400 + 8;
    auto keys = addOn("keys", true, true);
    keys["exp"] = ends;
    fx.seedStored(fx.token(withAddOns({ keys })));
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));
    const auto end = std::chrono::system_clock::time_point(std::chrono::seconds(ends));
    REQUIRE(controller.daysUntil(end) == 2);

    struct Changes : juce::ChangeListener
    {
        void changeListenerCallback(juce::ChangeBroadcaster*) override { ++count; }
        int count = 0;
    } changes;
    pumpFor(300); // what start() broadcast has landed
    controller.addChangeListener(&changes);

    pumpFor(2500); // a watch tick or so, with nothing to count down yet
    CHECK(changes.count == 0);

    REQUIRE(pumpUntil([&] { return controller.daysUntil(end) == 1; }, 15000));
    CHECK(pumpUntil([&] { return changes.count > 0; }, 6000)); // within a tick or two
    CHECK(controller.license().has_value()); // the day went by, nothing else changed
    CHECK(controller.screen() == Screen::Details);
    controller.removeChangeListener(&changes);
}

TEST_CASE("buySubProduct opens the add-on's buy link")
{
    controller_fixture fx;
    juce::StringArray opened;
    fx.config.openBrowser = [&](const juce::URL& url) { opened.add(url.toString(false)); return true; };
    fx.seedStored(fx.token(withAddOns({ addOn("pack", true) }))); // as the list has it
    ActivationController controller(fx.config, fx.makeLicensing());
    controller.start();
    REQUIRE(pumpUntil([&] { return controller.screen() == Screen::Details; }));

    CHECK_FALSE(controller.buySubProduct("strings")); // not listed yet

    fx.transport->responses.push_back(moonbase::http_response{ 200, {}, addOnList });
    bool done = false;
    controller.loadAddOns([&](bool) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));

    CHECK(controller.buySubProduct("strings"));
    CHECK(opened == juce::StringArray{ "https://demo.moonbase.sh/buy/demo-app.strings" });
    CHECK_FALSE(controller.buySubProduct("pack")); // owned, and not for sale
}

TEST_CASE("autoActivateSubProducts reaches the SDK options")
{
    ActivationConfig config;
    CHECK(config.toLicensingOptions().auto_activate_sub_products);
    config.autoActivateSubProducts = false;
    CHECK_FALSE(config.toLicensingOptions().auto_activate_sub_products);
    CHECK_FALSE(config.showAddOns); // the add-ons screen is opt-in
}

TEST_CASE("onActivationChanged reports an add-on trial that runs out")
{
    controller_fixture fx;
    auto strings = addOn("strings", true, true);
    strings["exp"] = now_seconds() + 2;
    fx.seedStored(fx.token(withAddOns({ strings })));
    ActivationController shared(fx.config, fx.makeLicensing());
    shared.start();
    REQUIRE(pumpUntil([&] { return shared.screen() == Screen::Details; }));

    ActivationComponent component(shared);
    std::vector<bool> reports;
    component.onActivationChanged = [&](bool active) { reports.push_back(active); };
    REQUIRE(pumpUntil([&] { return reports.size() == 1; }));

    REQUIRE(pumpUntil([&] { return reports.size() == 2; }, 8000));
    CHECK(reports.back());
    CHECK_FALSE(shared.hasSubProduct("strings"));
}

//==============================================================================
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui; // gives this thread a MessageManager

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int result = context.run();
    return result;
}
