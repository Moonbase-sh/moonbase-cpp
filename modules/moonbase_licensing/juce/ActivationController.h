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
// up within a couple of seconds.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
        Details,        // a valid full license is loaded
        UpdateAvailable,// a valid license, but a newer app version has been released
        Error           // an operation failed; statusMessage() has detail
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
    };

    explicit ActivationController(ActivationConfig config);

    // Test seam: drive the state machine against an injected licensing built
    // with fake store / transport / fingerprint. The primary constructor builds
    // the real dependencies from the config; this one takes them ready-made.
    // cancelInFlight (optional) is invoked on teardown to interrupt a blocking
    // request so workers can't outlive the controller (the real path wires it to
    // the HTTP transport's cancel()).
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
    void refreshLicense(bool force = true, std::function<void(bool refreshed)> onComplete = {});

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

    //== Offline activation ====================================================
    bool saveOfflineRequest(const juce::File& destination); // writes the device token
    void setOfflineResponse(const juce::File& responseFile);
    [[nodiscard]] bool hasOfflineResponse() const noexcept { return offlineResponse_ != juce::File(); }
    [[nodiscard]] juce::String offlineResponseName() const { return offlineResponse_.getFileName(); }
    void activateOffline();                // validate response locally + persist

    //== Deactivate / forget ===================================================
    void deactivate();   // server-side revoke (async); local forget fallback
    void clearLicense(); // local-only forget

    //== Navigation (screens not driven purely by license state) ===============
    void showWelcome();
    void showOffline();
    void showDetails();

    // Force a screen (with an optional synthetic license / offline error / lock
    // reason) with no network and no stored state. For previews, design iteration
    // and snapshot tests, not part of the normal activation flow.
    void setPreviewState(Screen screen,
                         std::optional<moonbase::license> license = std::nullopt,
                         juce::String previewError = {},
                         bool busy = false,
                         LockReason lockReason = LockReason::None);

    // Pin the wall clock used for trial-countdown math (trialDaysRemaining), so
    // previews and snapshot tests render a fixed number of days regardless of the
    // real date. Affects display only; no effect on the live activation flow. Pass
    // nullopt to unpin and fall back to the system clock.
    void setPreviewClock(std::optional<std::chrono::system_clock::time_point> now);

    // Force the UpdateAvailable screen with synthetic update state (no network),
    // for previews + snapshot tests. newVersion is read from the license's
    // current_release_version; currentVersion from the config's app version.
    void setPreviewUpdate(UpdateInfo::Phase phase,
                          moonbase::license license,
                          juce::String releaseNotes = {},
                          double progress = 0.0,
                          juce::String error = {},
                          bool canDownload = true);

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
    [[nodiscard]] Screen screenForCurrentLicense() const; // Welcome / Trial / Details
    void onActivationFulfilled(moonbase::license value);
    void endActivationFlows(); // stop polling, forget offline-flow progress, drop in-flight continuations

    //== License watch (no network) ============================================
    void watchLicense();                 // one tick: file changes, then deadlines
    [[nodiscard]] FileStamp stampLicenseFile() const; // watchedFile_ must be set
    void checkLicenseFile();
    void checkLicenseDeadlines();
    [[nodiscard]] bool activationInFlight() const noexcept;
    void onLicenseFileRemoved();
    void adoptStoredLicense(moonbase::license stored);
    void stopLicenseWatch();

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
    LockReason lockReason_ = LockReason::None;
    std::optional<moonbase::activation_request> pendingRequest_;
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

    // onLicenseChanged bookkeeping: the token last reported (nullopt = no license).
    bool licenseChangePending_ = false;
    bool licenseReported_ = false;
    std::optional<std::string> reportedToken_;

    // Bumped by every state-changing entry point; async continuations capture it
    // and no-op if it has moved on by the time they run on the message thread.
    std::atomic<std::uint64_t> generation_{0};

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
