#pragma once

// Headless activation state machine that drives the built-in UI by talking to
// the moonbase::licensing API directly (no juce::OnlineUnlockStatus). Network
// calls run on background threads; all state mutation + change notifications
// happen on the message thread, gated by a generation counter so a slow request
// can never clobber a newer state (cancel, a fresh activation, deactivate).
//
// Observe it as a juce::ChangeBroadcaster: on every change, read screen() and
// license() and repaint.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <moonbase/moonbase.hpp>

#include <juce_events/juce_events.h>

#include "ActivationConfig.h"

namespace moonbase::juce_integration {

class ActivationController : private juce::Timer,
                             public juce::ChangeBroadcaster
{
public:
    enum class Screen
    {
        Loading,     // validating any stored license at startup
        Welcome,     // not activated — choose online / trial / offline
        BrowserWait, // browser activation in progress, polling
        Success,     // just activated
        Offline,     // offline request/response file flow
        Trial,       // a valid trial license is loaded
        Expired,     // a trial license that has ended (plugin locked; see expiredTrial())
        Details,     // a valid full license is loaded
        Error        // an operation failed; statusMessage() has detail
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

    //== Lifecycle =============================================================
    // Loads + validates any stored license and routes to the right screen.
    void start();

    //== Online activation =====================================================
    void beginOnlineActivation();          // request + open browser + poll
    void cancelActivation();               // stop polling, back to Welcome

    //== Re-validation =========================================================
    // Re-check the current license against the server and refresh its entitlements
    // (sub-product ownership, properties, expiry, seats) in place. Call this after
    // a purchase so newly granted features load without an app restart. Runs async
    // and silently (no screen change); on success the license updates and
    // onActivationChanged fires so you can reload features. `force` bypasses the
    // online-validation throttle (use it right after a purchase). No-op for offline
    // licenses. The optional callback runs on the message thread with the outcome.
    void refreshLicense(bool force = true, std::function<void(bool refreshed)> onComplete = {});

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

    // Force a screen (with an optional synthetic license / offline error) with no
    // network and no stored state. For previews, design iteration and snapshot
    // tests — not part of the normal activation flow.
    void setPreviewState(Screen screen,
                         std::optional<moonbase::license> license = std::nullopt,
                         juce::String previewError = {},
                         bool busy = false);

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
    [[nodiscard]] juce::String statusMessage() const { return statusMessage_; }
    [[nodiscard]] juce::String offlineError() const { return offlineError_; }
    [[nodiscard]] juce::String deviceLabel() const { return deviceLabel_; }
    [[nodiscard]] const ActivationConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool isBusy() const noexcept { return busy_; }
    [[nodiscard]] bool offlineRequestSaved() const noexcept { return offlineRequestSaved_; }

    // Trial display helpers (valid only when a trial license is loaded).
    [[nodiscard]] int trialDaysRemaining() const;

private:
    void timerCallback() override;

    void setScreen(Screen newScreen, const juce::String& message = {});
    void setLicense(std::optional<moonbase::license> value); // updates license_ + licensedFlag()
    void applyLicense(std::optional<moonbase::license> value);
    void showTrialExpired(moonbase::license expired); // locks + routes to the Expired screen
    [[nodiscard]] Screen screenForCurrentLicense() const; // Welcome / Trial / Details
    void onActivationFulfilled(moonbase::license value);
    void deleteStoredMatching(const juce::String& activationId);
    void deleteStoredLicense(); // best-effort delete of the local license file
    void setDeviceLabel(juce::String deviceName);
    void emitDiagnostic(const juce::String& message); // message-thread only
    bool ensureReady(); // false (+ Error screen) when misconfigured / not built

    static juce::String shortPlatformName();

    ActivationConfig config_;
    std::shared_ptr<moonbase::licensing> licensing_;

    // Network/file work runs here instead of detached threads, so the destructor
    // can drain workers (after cancelInFlight_ unblocks any in-flight request);
    // nothing outlives the controller.
    std::function<void()> cancelInFlight_;
    juce::ThreadPool threadPool_ { 2 };

    Screen screen_ = Screen::Loading;
    std::optional<moonbase::license> license_;
    std::atomic<bool> licensed_{false}; // mirror of license_.has_value() for the audio thread
    std::optional<moonbase::license> expiredTrial_; // display-only backing for the Expired screen
    std::optional<moonbase::activation_request> pendingRequest_;

    juce::String statusMessage_;
    juce::String offlineError_;
    juce::String deviceLabel_;
    juce::String configError_; // non-empty when the config failed validation/build
    juce::File offlineResponse_;
    bool offlineRequestSaved_ = false;

    bool busy_ = false;
    bool pollInFlight_ = false;

    // Bumped by every state-changing entry point; async continuations capture it
    // and no-op if it has moved on by the time they run on the message thread.
    std::atomic<std::uint64_t> generation_{0};

    JUCE_DECLARE_WEAK_REFERENCEABLE(ActivationController)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivationController)
};

} // namespace moonbase::juce_integration
