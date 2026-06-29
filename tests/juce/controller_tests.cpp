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

#include <functional>

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

bool settled(const ActivationController& c)
{
    return c.screen() != Screen::Loading;
}

struct controller_fixture
{
    moonbase::tests::generated_key key = moonbase::tests::generate_key();
    std::shared_ptr<moonbase::static_fingerprint_provider> fingerprint =
        std::make_shared<moonbase::static_fingerprint_provider>("Studio Mac", "device-id");
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
        store = std::make_shared<moonbase::file_license_store>(
            std::filesystem::path(licenseFile.getFullPathName().toStdString()));

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

//==============================================================================
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui; // gives this thread a MessageManager

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int result = context.run();
    return result;
}
