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
