#pragma once

// Headless activation state machine that drives the built-in UI by talking to
// the moonbase::licensing API directly (no juce::OnlineUnlockStatus). Network
// calls run on background threads; all state mutation + change notifications
// happen on the message thread, gated by a generation counter so a slow request
// can never clobber a newer state (cancel, a fresh activation, deactivate).
//
// Observe it as a juce::ChangeBroadcaster: on every change, read screen() and
// license() and repaint. To hear only about the license itself, use
// onLicenseChanged.
//
// Once started, it also watches the license on its own, with no network
// traffic: a license whose `exp` passes or whose offline grace period runs out
// is re-checked (and locked if it has ended) without a restart, and a license
// another plugin instance or process activates, refreshes or removes is picked
// up within a couple of seconds. A change is broadcast, too, when a day goes by
// on a countdown to when the license or an add-on ends, so a screen showing the
// days left keeps up.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <moonbase/moonbase.hpp>

#include <juce_events/juce_events.h>

#include "ActivationConfig.h"
#include "ActivationState.h"
#include "WorkerPool.h"

namespace moonbase::juce_integration {

class ActivationController : private juce::Timer,
                             private juce::URL::DownloadTaskListener,
                             public juce::ChangeBroadcaster
{
public:
    enum class Screen
    {
        Loading,        // validating any stored license at startup
        Welcome,        // not activated — choose online / trial / offline
        BrowserWait,    // browser activation in progress, polling
        Success,        // just activated
        Offline,        // offline request/response file flow
        Trial,          // a valid trial license is loaded
        Expired,        // a trial license that has ended (plugin locked; see expiredTrial())
        NotCovered,     // a license that doesn't cover this version of the app (plugin locked; see notCovered())
        Details,        // a valid full license is loaded
        UpdateAvailable,// a valid license, but a newer app version has been released
        Error,          // an operation failed; statusMessage() has detail
        AddOns          // the product's add-ons, from a full license (see addOns())
    };

    // State backing the "Update available" screen. The version strings are known
    // immediately (from the license claim + app version); releaseNotes and the
    // download URL are fetched from the inventory endpoints, so the view tracks a
    // small phase machine while that happens.
    struct UpdateInfo
    {
        enum class Phase
        {
            Loading,    // fetching release notes
            Ready,      // notes loaded; download not started (error set if the fetch failed)
            Downloading,// installer download in progress (see progress)
            Done        // installer downloaded + revealed
        };

        Phase phase = Phase::Loading;
        juce::String currentVersion;  // the running app version
        juce::String newVersion;      // the released version from the license claim
        juce::String releaseNotes;    // plain text; empty until loaded
        juce::String error;           // non-empty when a fetch/download failed
        double progress = 0.0;        // 0..1 while Downloading
        // Whether this license may download the installer, derived from the
        // product's release access-control level. False e.g. for a trial when the
        // product restricts downloads to owners. Known once notes have loaded.
        bool canDownload = true;
        // False when Moonbase says the customer's licenses don't cover the new
        // version (canDownload is false then too): it needs an upgrade, bought
        // at upgradeUrl (empty when the store sells none). Known once notes
        // have loaded.
        bool covered = true;
        std::optional<moonbase::license_release_access> releaseAccess; // what the licenses cover
        juce::String upgradeUrl;
    };

    // State backing the NotCovered screen: a license that is valid, but doesn't
    // cover the version of the app that runs, such as one with 12 months of
    // updates that ended before this version came out. The plugin is locked,
    // but the license stays stored, since it still covers the versions it did.
    struct NotCoveredInfo
    {
        moonbase::license license; // for display; the stored license
        juce::String appVersion;   // the version it doesn't cover, MAJOR.MINOR.PATCH (empty when unknown)
        std::optional<moonbase::license_release_access> releaseAccess; // what it covers
        // Where the customer buys the upgrade or renewal. Only Moonbase knows it,
        // so it is empty when the license was refused on this computer (offline,
        // or checked online moments ago), and when the store sells none.
        juce::String upgradeUrl;
    };

    // State backing the add-ons screen: the product's add-ons the customer can do
    // something with, as moonbase::licensing::list_sub_products reports them for
    // this device, and the add-on an activation or trial is running for.
    struct AddOnsInfo
    {
        enum class Phase
        {
            Idle,    // not loaded
            Loading, // listing; addOns keeps what an earlier load found
            Ready    // listed, or failed (error set)
        };

        Phase phase = Phase::Idle;
        juce::String currency;                                // the currency to show prices in
        std::vector<moonbase::sub_product_catalog_entry> addOns;
        juce::String error;       // non-empty when listing failed; for the user
        juce::String busyId;      // the add-on an activation or trial is running for
        juce::String actionError; // why the last activation or trial failed; for the user
    };

    explicit ActivationController(ActivationConfig config);

    // Test seam: drive the state machine against an injected licensing built
    // with fake store / transport / fingerprint. The primary constructor builds
    // the real dependencies from the config; this one takes them ready-made.
    // cancelInFlight (optional) is invoked on teardown to interrupt a blocking
    // request or lock wait so workers can't outlive the controller (the real path
    // wires it to the HTTP transports' cancel() and the store's
    // cancel_lock_waits()).
    ActivationController(ActivationConfig config,
                         std::shared_ptr<moonbase::licensing> licensing,
                         juce::String deviceName = "Test Device",
                         std::function<void()> cancelInFlight = {});

    ~ActivationController() override;

    // Fired on the message thread once start() has settled, then whenever the
    // license itself changes: activated, refreshed into a new token, picked up
    // from another instance, revoked, expired or cleared. Not fired for screen
    // changes, progress or a re-check that changed nothing. `licensed` matches
    // licensedFlag(); read license() for the rest. Assign it before start().
    std::function<void(bool licensed)> onLicenseChanged;

    //== Lifecycle =============================================================
    // Loads + validates any stored license, routes to the right screen and
    // starts watching the license (see the class comment).
    void start();

    //== Online activation =====================================================
    void beginOnlineActivation();          // request + open browser + poll
    void cancelActivation();               // stop polling, back to Welcome

    // The browser link for the online activation in progress, so a custom UI can
    // show or copy it when the browser didn't open. Empty until the request has
    // been created (a change is broadcast when it arrives), and again once the
    // activation completes or is cancelled.
    [[nodiscard]] juce::String pendingBrowserUrl() const;

    // True while that link is waiting but the browser couldn't be opened for it:
    // the system has no default browser, say, or config.openBrowser returned
    // false. The built-in screen then asks the user to copy the link instead.
    [[nodiscard]] bool browserOpenFailed() const noexcept { return pendingRequest_.has_value() && browserOpenFailed_; }

    //== Re-validation =========================================================
    // Re-check the current license against the server and refresh its entitlements
    // (sub-product ownership, properties, expiry, seats) in place. Call this after
    // a purchase so newly granted features load without an app restart. Runs async;
    // on success the license updates, and onLicenseChanged / onActivationChanged
    // fire when the server sent a new token, so you can reload features. `force`
    // bypasses the online-validation throttle (use it right after a purchase).
    // A revoked or lapsed license locks, and so does one that can't be checked
    // once the offline grace period has run out; within it a network failure
    // keeps the license. No-op for offline licenses, and while an online
    // activation is in progress (it brings a fresh license of its own). The
    // optional callback runs on the message thread with the outcome.
    //
    // On the NotCovered screen it re-checks the license that doesn't cover this
    // version, so once the customer has bought the upgrade it unlocks. While it
    // runs, isRefreshing() is true.
    void refreshLicense(bool force = true, std::function<void(bool refreshed)> onComplete = {});

    // True while a refreshLicense() call is waiting for its answer.
    [[nodiscard]] bool isRefreshing() const noexcept { return refreshesInFlight_ > 0; }

    //== Licenses that cover only some versions =================================
    // Offer the upgrade the screen calls for. On the NotCovered screen: open the
    // upgrade page Moonbase named, or, without one, start an online activation,
    // whose browser page offers the upgrade where the store sells one and
    // activates this computer once a license covers this version. On the update
    // screen for a version the license doesn't cover: open its upgrade page.
    // After the purchase, refreshLicense() picks up the upgraded license.
    void upgradeLicense();

    //== App update flow =======================================================
    // True when a valid license is loaded, enableUpdatePrompt is set, and the
    // license's current_release_version claim is a higher semver than the running
    // app version. Pure (no network), safe to call any time.
    [[nodiscard]] bool updateAvailable() const;

    // Start the in-app installer download for the released version (resolves an
    // authenticated URL from the inventory endpoint, then downloads with progress).
    // No-op unless the UpdateAvailable screen is showing. Observe updateInfo().
    void startUpdateDownload();

    // Re-open the update screen on demand. fromLicenseView marks that the user got
    // here from the license view's "Update available" badge (vs an automatic
    // open/focus presentation), which controls where "Skip this update" returns to.
    // Re-fetches the release notes. No-op when no update is available.
    void showUpdate(bool fromLicenseView = false);

    // True when the update screen was opened from the license view (the badge), so
    // dismissing should return there rather than close an auto-presented overlay.
    [[nodiscard]] bool updateCameFromLicenseView() const noexcept { return updateFromLicenseView_; }

    // Re-reveal the already-downloaded installer in the OS file browser (the
    // "Done" state of the update flow).
    void revealUpdateDownload();

    // "Skip this update": leave the update screen for the normal Details / Trial
    // screen and don't prompt for this version again. The skipped version is
    // persisted in the state file, so it holds across restarts; a newer release
    // still prompts.
    void dismissUpdate();

    //== Add-ons ===============================================================
    // An add-on (a sub-product, with a product ID like "my-app.drum-kit") that the
    // merchant includes in the product's license travels in it, so the loaded
    // license already says which ones this device may unlock. Read them on the
    // message thread, from onLicenseChanged, which also fires when an add-on's
    // trial runs out while the license itself carries on, and on the audio
    // thread through subProductFlag().

    // Whether to unlock the add-on here: the loaded license lists it as active
    // and it hasn't expired. Takes the add-on's own part of its product ID
    // ("drum-kit") or its whole ID. False while no license is loaded.
    //
    // Message thread only, as is activeSubProducts(): both read the license held
    // there, and allocate. Not for processBlock: use subProductFlag() there.
    // Debug builds assert when called from another thread.
    [[nodiscard]] bool hasSubProduct(const juce::String& subProductId) const;

    // The add-ons hasSubProduct() is true for right now, by their own part of
    // their product ID.
    [[nodiscard]] juce::StringArray activeSubProducts() const;

    // hasSubProduct(subProductId) for the audio thread, as licensedFlag() is for
    // the license: ask for it once on the message thread (in the processor's
    // constructor, say), keep the pointer, and load() it in processBlock. Never
    // null, and valid for the controller's lifetime. It changes in the same step
    // as licensedFlag(), and within a couple of seconds of an add-on running out
    // while the license carries on. One the license doesn't include stays false.
    [[nodiscard]] const std::atomic<bool>* subProductFlag(const juce::String& subProductId) const;

    // Takes a seat of the add-on for this device, or starts its trial, from the
    // full license loaded, then loads and stores the license that comes back and
    // lists the add-ons again if they were loaded. Activating is for an add-on
    // the customer owns: one whose seat was revoked here, or every one when
    // config.autoActivateSubProducts is off. A trial is for one they don't own
    // (owning it, they get a seat instead).
    //
    // Async: onComplete runs on the message thread with whether it worked and,
    // when it didn't, a message to show the user (addOns().actionError has it
    // too, unless the license changed in the meantime, and onDiagnostic the
    // detail). One at a time: a call while another runs (addOns().busyId) is
    // refused. A license the server no longer accepts is re-checked, and locks as
    // refreshLicense() would.
    void activateSubProduct(const juce::String& subProductId,
                            std::function<void(bool activated, const juce::String& message)> onComplete = {});
    void requestSubProductTrial(const juce::String& subProductId,
                                std::function<void(bool started, const juce::String& message)> onComplete = {});

    // Lists the product's add-ons for this device into addOns(), with their
    // prices, ownership and trials, for a store or upgrade screen. Async; a change
    // is broadcast when it lands, and onComplete runs then.
    //
    // Can take a seat: not read-only while config.autoActivateSubProducts is on
    // (the default). The list itself changes nothing on the server, but when it
    // shows an add-on the customer owns, with a seat free, that the license
    // doesn't hold yet (one bought since the license was last checked), the
    // license is re-checked once, which takes that seat as every check does, and
    // the add-ons are listed again. With the option off it only reads.
    void loadAddOns(std::function<void(bool loaded)> onComplete = {});

    // Re-validates the license, so an add-on bought since takes its seat, then
    // lists the add-ons again. What the add-ons screen's "Refresh" does after a
    // purchase in the browser.
    void refreshAddOns();

    // Opens the add-on's buy link (from addOns()) in the browser, through
    // config.openBrowser when set. False when it has none, or it couldn't open.
    //
    // When the app then goes to the background (the customer is in the browser)
    // and comes back to the front, the add-ons are refreshed as refreshAddOns()
    // does, so one just bought unlocks without a click. Where the platform can't
    // tell that the app is in the background, "Refresh" is the way.
    bool buySubProduct(const juce::String& subProductId);

    [[nodiscard]] const AddOnsInfo& addOns() const noexcept { return addOns_; }

    //== Offline activation ====================================================
    bool saveOfflineRequest(const juce::File& destination); // writes the device token
    void setOfflineResponse(const juce::File& responseFile);
    [[nodiscard]] bool hasOfflineResponse() const noexcept { return offlineResponse_ != juce::File(); }
    [[nodiscard]] juce::String offlineResponseName() const { return offlineResponse_.getFileName(); }
    void activateOffline();                // validate response locally + persist

    //== Deactivate / forget ===================================================
    void deactivate();   // server-side revoke (async); local forget fallback. Also hands
                         // back the seat of a license that doesn't cover this version
    void clearLicense(); // local-only forget

    //== Navigation (screens not driven purely by license state) ===============
    void showWelcome();
    void showOffline();
    void showDetails();
    void showAddOns(); // a full license only: routes to Screen::AddOns and loads them

    // Force a screen (with an optional synthetic license / offline error / lock
    // reason) with no network and no stored state. For previews, design iteration
    // and snapshot tests, not part of the normal activation flow.
    void setPreviewState(Screen screen,
                         std::optional<moonbase::license> license = std::nullopt,
                         juce::String previewError = {},
                         bool busy = false,
                         LockReason lockReason = LockReason::None);

    // Give a previewed BrowserWait screen its browser link, as if the activation
    // request had arrived, and say whether the browser opened for it. Call after
    // setPreviewState(Screen::BrowserWait).
    void setPreviewBrowserLink(juce::String browserUrl, bool opened = true);

    // Pin the wall clock used for trial-countdown math (trialDaysRemaining,
    // daysUntil), add-on expiry (hasSubProduct, activeSubProducts) and the dates
    // the built-in screens describe (displayClock), so previews and snapshot tests
    // render a fixed number of days regardless of the real date. For previews
    // only, which stop the license watch; validation keeps the real clock. Pass
    // nullopt to unpin and fall back to the system clock.
    void setPreviewClock(std::optional<std::chrono::system_clock::time_point> now);

    // The clock the built-in screens judge dates by: the pinned preview clock,
    // else the system clock.
    [[nodiscard]] std::chrono::system_clock::time_point displayClock() const { return clockNow(); }

    // Force the UpdateAvailable screen with synthetic update state (no network),
    // for previews + snapshot tests. newVersion is read from the license's
    // current_release_version; currentVersion from the config's app version.
    // covered = false previews a new version the license doesn't cover (its
    // release access comes from the license).
    void setPreviewUpdate(UpdateInfo::Phase phase,
                          moonbase::license license,
                          juce::String releaseNotes = {},
                          double progress = 0.0,
                          juce::String error = {},
                          bool canDownload = true,
                          bool covered = true,
                          juce::String upgradeUrl = {});

    // Force the NotCovered screen for a license that doesn't cover the config's
    // app version (no network), for previews + snapshot tests. Its release
    // access comes from the license.
    void setPreviewNotCovered(moonbase::license license, juce::String upgradeUrl = {});

    // Force the AddOns screen with a synthetic license and add-on list (no
    // network), for previews + snapshot tests.
    void setPreviewAddOns(moonbase::license license, AddOnsInfo info);

    // Replace how the controller tells whether the app is in front of the user,
    // which is juce::Process::isForegroundProcess otherwise. For tests of the
    // refresh after a purchase in the browser (see buySubProduct).
    void setForegroundCheck(std::function<bool()> isInForeground);

    //== Accessors =============================================================
    [[nodiscard]] Screen screen() const noexcept { return screen_; }
    [[nodiscard]] const std::optional<moonbase::license>& license() const noexcept { return license_; }
    // Audio-thread-safe "is there a valid license" flag, updated on the message
    // thread whenever the license changes. Read it from processBlock for gating
    // without a ChangeListener: `if (! controller.licensedFlag().load()) ...`.
    [[nodiscard]] const std::atomic<bool>& licensedFlag() const noexcept { return licensed_; }
    // The ended trial backing the Expired screen. license() stays empty in this
    // state (the plugin is locked); this is for display only.
    [[nodiscard]] const std::optional<moonbase::license>& expiredTrial() const noexcept { return expiredTrial_; }
    // The license backing the NotCovered screen, and what it covers. license()
    // stays empty in this state (the plugin is locked). Set from the moment a
    // license turns out not to cover this version until a license is loaded or
    // the plugin locks for another reason.
    [[nodiscard]] const std::optional<NotCoveredInfo>& notCovered() const noexcept { return notCovered_; }
    // Why the license was lost: set when a license is (or a stored one turns out
    // to be) deactivated, expired, rejected or unverifiable, and back to None as
    // soon as a license is loaded. The built-in welcome screen explains it; read
    // it from onLicenseChanged(false) to do the same in a custom UI.
    [[nodiscard]] LockReason lockReason() const noexcept { return lockReason_; }
    [[nodiscard]] juce::String statusMessage() const { return statusMessage_; }
    [[nodiscard]] juce::String offlineError() const { return offlineError_; }
    [[nodiscard]] juce::String deviceLabel() const { return deviceLabel_; }

    // How this machine's device id was derived: the id itself, the fingerprint
    // spec version, the platform tag and the *names* of the identity parameters
    // that contributed. Safe to log or put behind a "Copy diagnostics" button;
    // parameter values are hardware serial numbers and are never exposed.
    //
    // Empty when the controller is misconfigured, when a custom resolver does not
    // describe itself, or when this machine has no readable identity.
    [[nodiscard]] std::optional<moonbase::device_id_description> describeDevice() const;

    [[nodiscard]] const ActivationConfig& config() const noexcept { return config_; }
    [[nodiscard]] const UpdateInfo& updateInfo() const noexcept { return updateInfo_; }
    [[nodiscard]] bool isBusy() const noexcept { return busy_; }
    [[nodiscard]] bool offlineRequestSaved() const noexcept { return offlineRequestSaved_; }

    // Trial display helpers (valid only when a trial license is loaded).
    [[nodiscard]] int trialDaysRemaining() const;
    // Whole days from now until `when`, rounded up, and 0 once it has passed. On
    // the preview clock when one is pinned, as trialDaysRemaining() is. For
    // display, such as an add-on trial's countdown.
    [[nodiscard]] int daysUntil(std::chrono::system_clock::time_point when) const;
    // True when the loaded trial has no end date: the product's trials never
    // expire, so its token carries no expiry. trialDaysRemaining() is 0 then, so
    // show no countdown.
    [[nodiscard]] bool trialIsEndless() const;

private:
    // A message-thread timer with a callback. The controller's own Timer base
    // is the activation poll, so the license watch needs a second one.
    class CallbackTimer : public juce::Timer
    {
    public:
        std::function<void()> onTick;
        void timerCallback() override
        {
            if (onTick)
                onTick();
        }
    };

    // What the license watch last saw of the license file.
    struct FileStamp
    {
        bool exists = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        bool operator==(const FileStamp& other) const
        {
            return exists == other.exists && size == other.size && modified == other.modified;
        }
    };

    void timerCallback() override;

    // juce::URL::DownloadTaskListener: both arrive on a background thread and
    // marshal to the message thread (gated by downloadGeneration_).
    void finished(juce::URL::DownloadTask* task, bool success) override;
    void progress(juce::URL::DownloadTask* task, juce::int64 bytesDownloaded,
                  juce::int64 totalLength) override;

    void beginUpdateFlow(Screen destination); // sets UpdateInfo + fetches notes
    void fetchUpdateInfo();
    // Whether the current license may download the installer, given the product's
    // release access-control flags (needs-user / needs-ownership).
    [[nodiscard]] bool licenseCanDownload(const moonbase::release_info& info) const;
    void beginFileDownload(const moonbase::download_target& target);
    void failUpdateDownload(const juce::String& message);
    [[nodiscard]] juce::String defaultInstallerName() const;
    // True when the loaded license's released version is in the persisted
    // ignored-updates list (a newer version still prompts).
    [[nodiscard]] bool updateDismissedForCurrentLicense() const;
    [[nodiscard]] juce::File stateFilePath() const;

    void setScreen(Screen newScreen, const juce::String& message = {});
    void setLicense(std::optional<moonbase::license> value); // updates license_ + licensedFlag()
    void applyLicense(std::optional<moonbase::license> value);
    void lock(LockReason reason); // drop the license and route to Welcome, saying why
    void showTrialExpired(moonbase::license expired); // locks + routes to the Expired screen
    void showNotCovered(NotCoveredInfo info);          // locks, keeps the license stored + routes to NotCovered
    [[nodiscard]] static NotCoveredInfo notCoveredFrom(const moonbase::release_not_covered_error& error,
                                                       const moonbase::license& fallback);
    void storeLicense(const moonbase::license& value, const char* context); // best-effort persist under the lock
    bool openBrowser(const juce::URL& url); // config.openBrowser, else the default browser
    [[nodiscard]] Screen screenForCurrentLicense() const; // Welcome / Trial / Details
    void onActivationFulfilled(moonbase::license value);
    void endActivationFlows(); // stop polling, forget offline-flow progress, drop in-flight continuations

    //== License watch (no network) ============================================
    void watchLicense();                 // one tick: file changes, then deadlines
    [[nodiscard]] FileStamp stampLicenseFile() const; // watchedFile_ must be set
    void checkLicenseFile();
    void checkLicenseDeadlines();
    void checkCountdowns();
    [[nodiscard]] bool activationInFlight() const noexcept;
    void onLicenseFileRemoved();
    void adoptStoredLicense(moonbase::license stored);
    void adoptNotCovered(NotCoveredInfo info); // a stored license that doesn't cover this version: locks
    // True when `copy` is an older copy of the activation held (licensed or not covered).
    [[nodiscard]] bool isOlderThanHeld(const moonbase::license& copy) const;
    void stopLicenseWatch();

    void resetAddOns(); // the license they were for went or was replaced
    void publishSubProductFlags();
    void listAddOns(bool pickUpSeats, std::function<void(bool)> onComplete);
    void refreshAndListAddOns();
    [[nodiscard]] bool listsSeatToTake() const; // see loadAddOns
    void checkPurchaseReturn();                 // a license watch tick; see buySubProduct
    void runSubProductAction(const juce::String& subProductId, bool trial,
                             std::function<void(bool, const juce::String&)> onComplete);
    [[nodiscard]] juce::String addOnName(const juce::String& subProductId) const;
    [[nodiscard]] std::chrono::system_clock::time_point clockNow() const; // the preview clock when pinned

    void notifyLicenseChange();  // coalesced, async onLicenseChanged
    void deliverLicenseChange();
    void deleteStoredMatching(const juce::String& activationId);
    void deleteStoredLicense(); // best-effort delete of the local license file
    void setDeviceLabel(juce::String deviceName);
    void emitDiagnostic(const juce::String& message); // message-thread only
    bool ensureReady(); // false (+ Error screen) when misconfigured / not built

    static juce::String shortPlatformName();

    ActivationConfig config_;
    std::shared_ptr<moonbase::licensing> licensing_;
    // Built only on the real path (primary constructor); empty when the controller
    // was handed a ready-made licensing (test seam). Guards the update flow.
    std::optional<moonbase::inventory_client> inventory_;

    // Network/file work runs here instead of detached threads, so the destructor
    // can drain workers (after cancelInFlight_ unblocks any in-flight request);
    // nothing outlives the controller. A module-owned pool rather than a
    // juce::ThreadPool, which can kill a thread at teardown (see WorkerPool.h).
    std::function<void()> cancelInFlight_;
    detail::WorkerPool threadPool_ { 2 };

    Screen screen_ = Screen::Loading;
    std::optional<moonbase::license> license_;
    std::atomic<bool> licensed_{false}; // mirror of license_.has_value() for the audio thread
    std::optional<moonbase::license> expiredTrial_; // display-only backing for the Expired screen
    std::optional<NotCoveredInfo> notCovered_;      // display-only backing for the NotCovered screen
    LockReason lockReason_ = LockReason::None;
    std::optional<moonbase::activation_request> pendingRequest_;
    bool browserOpenFailed_ = false; // for pendingRequest_; set with it
    std::optional<std::chrono::system_clock::time_point> previewClock_; // pinned "now" for previews/snapshots (trialDaysRemaining)

    juce::String statusMessage_;
    juce::String offlineError_;
    juce::String deviceLabel_;
    juce::String configError_; // non-empty when the config failed validation/build
    juce::File offlineResponse_;
    bool offlineRequestSaved_ = false;

    bool busy_ = false;
    bool pollInFlight_ = false;

    //== License watch =========================================================
    CallbackTimer licenseWatch_;
    std::optional<std::filesystem::path> watchedFile_; // empty when the store is not a file
    std::optional<FileStamp> watchedStamp_;            // last look; taken first in start()
    int refreshesInFlight_ = 0; // refreshLicense() calls whose answer hasn't landed yet
    // When the watch last asked the server about a license past its grace
    // period. Spaced by onlineCheckInterval, so a skewed clock can't turn the
    // watch into a request every tick.
    std::optional<std::chrono::steady_clock::time_point> lastGraceCheck_;
    // The ends the screens count down to (see checkCountdowns), and the days to
    // each as of the last watch tick.
    std::vector<std::chrono::system_clock::time_point> countdownEnds_;
    std::vector<int> countdownDays_;

    // onLicenseChanged bookkeeping: the token last reported (nullopt = no license).
    bool licenseChangePending_ = false;
    bool licenseReported_ = false;
    std::optional<std::string> reportedToken_;
    juce::StringArray reportedSubProducts_; // activeSubProducts() as last reported

    //== Add-ons ===============================================================
    AddOnsInfo addOns_;
    // subProductFlag()'s flags, by the ID each was asked for. A map's entries
    // never move, so the pointers handed out stay valid.
    mutable std::map<juce::String, std::atomic<bool>> subProductFlags_;
    // Bumped by every list, and when the license they were listed for goes, so a
    // list that lands late is dropped.
    std::atomic<std::uint64_t> addOnsGeneration_{0};
    std::uint64_t addOnCalls_ = 0;   // numbers each add-on activation or trial
    std::uint64_t addOnCall_ = 0;    // the one busyId belongs to; 0 when none
    // Set by buySubProduct once the browser opened, until the app comes back to
    // the front (or an hour has passed).
    std::optional<std::chrono::steady_clock::time_point> purchaseOpenedAt_;
    bool leftForPurchase_ = false; // the app has been in the background since
    juce::StringArray lapsesChecked_; // add-ons that ran out and were re-checked for, once each (see checkLicenseDeadlines)
    std::function<bool()> isInForeground_; // empty: juce::Process::isForegroundProcess
    // Held by a refresh and an add-on call for the whole of their work, from the
    // request until the answer is stored and queued, so the answers land here in
    // the order they reached the store. The store's own lock, which they also
    // hold from the request until the write, orders them against other
    // instances. Shared with the workers, which outlive no one: the destructor
    // joins them.
    std::shared_ptr<std::mutex> onlineCalls_ = std::make_shared<std::mutex>();

    // Bumped by every state-changing entry point; async continuations capture it
    // and no-op if it has moved on by the time they run on the message thread.
    // Shared with the workers, which look at it under the store's lock before
    // storing an answer, so a superseded one never reaches the store.
    std::shared_ptr<std::atomic<std::uint64_t>> generations_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::atomic<std::uint64_t>& generation_ = *generations_;

    //== Update flow ===========================================================
    UpdateInfo updateInfo_;
    // True when the update screen was entered from the license-view badge (vs an
    // automatic open/focus presentation); controls dismissal navigation.
    bool updateFromLicenseView_ = false;
    // Persisted client state (ignored update versions, and room to grow). Built
    // once the config is valid; empty only on a misconfigured controller.
    std::optional<ActivationState> state_;
    // Separate generation for the update flow's async work (notes fetch + file
    // download) so a license re-validation can't cancel an in-flight download.
    std::atomic<std::uint64_t> updateGeneration_{0};
    std::atomic<std::uint64_t> downloadGeneration_{0};
    std::unique_ptr<juce::URL::DownloadTask> updateDownload_;
    juce::File updateFile_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(ActivationController)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivationController)
};

} // namespace moonbase::juce_integration
