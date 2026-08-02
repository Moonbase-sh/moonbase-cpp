// Implementation of ActivationController. Compiled as part of the single
// moonbase_licensing module translation unit.

#include "ActivationController.h"
#include "juce_http_transport.h"

namespace moonbase::juce_integration {

namespace {
constexpr int kPollIntervalMs = 1500;

// Diagnostic-only error text. For transport failures (moonbase::api_error) the
// SDK stashes actionable guidance (e.g. the macOS network entitlement hint) in
// the detail field; append it so it reaches onDiagnostic without ever appearing
// in the friendly, user-facing screen text.
juce::String describeError(const std::exception& ex)
{
    juce::String message = ex.what();
    if (const auto* api = dynamic_cast<const moonbase::api_error*>(&ex))
        if (! api->detail().empty())
            message << " (" << api->detail() << ")";

    // Every activation and offline-token path funnels through here, and this one
    // is not a Moonbase failure at all: the machine has no stable hardware
    // identifier to hash, so there is nothing to activate against. Left bare it
    // reads like a bug in the plugin, so point at the two real options.
    if (dynamic_cast<const moonbase::insufficient_device_identity_error*>(&ex) != nullptr)
        message << ". This machine has no stable hardware identifier to bind a license to."
                   " On a virtual machine or a container, check that it has a system UUID or a"
                   " machine-id; otherwise set ActivationConfig::allowDeviceNameFallback to accept"
                   " a weaker id based on the computer name.";

    return message;
}
} // namespace

ActivationController::ActivationController(ActivationConfig config)
    : config_(std::move(config))
{
    // Fail loud, not late: a missing/malformed endpoint, product id or public
    // key becomes a clear Error state instead of a confusing "license invalid"
    // (or a throw out of the consumer's component constructor) further down.
    configError_ = config_.validate();
    if (configError_.isNotEmpty())
    {
        screen_ = Screen::Error;
        statusMessage_ = configError_;
        return; // licensing_ stays null; entry points guard via ensureReady()
    }

    const auto file = config_.resolvedLicenseFile();
    file.getParentDirectory().createDirectory();

    auto store = std::make_shared<moonbase::file_license_store>(
        std::filesystem::path(file.getFullPathName().toStdString()));
    auto deviceIds = config_.resolvedDeviceIdResolver();
    auto transport = std::make_shared<juce_http_transport>();
    // A second transport for the inventory (update) calls, so an update download
    // URL fetch and a license validation can't block on each other's stream.
    auto inventoryTransport = std::make_shared<juce_http_transport>();

    try
    {
        licensing_ = std::make_shared<moonbase::licensing>(
            config_.toLicensingOptions(), std::move(store), deviceIds, transport);
    }
    catch (const std::exception& ex)
    {
        // The SDK parses the public key on construction; surface a bad key here.
        configError_ = juce::String("Invalid activation configuration: ") + ex.what();
        screen_ = Screen::Error;
        statusMessage_ = configError_;
        return;
    }

    inventory_.emplace(config_.toLicensingOptions(), inventoryTransport);

    cancelInFlight_ = [transport, inventoryTransport]
    {
        transport->cancel();
        inventoryTransport->cancel();
    };
    // A custom resolver's device_name() is arbitrary consumer code, and this
    // constructor runs inside a plugin editor's constructor, where an escaping
    // exception takes the host down with it. An empty label is fine:
    // setDeviceLabel renders that as "This device".
    juce::String resolvedDeviceName = config_.deviceName;
    if (resolvedDeviceName.isEmpty())
    {
        try
        {
            resolvedDeviceName = juce::String(deviceIds->device_name());
        }
        catch (const std::exception&)
        {
        }
    }
    setDeviceLabel(std::move(resolvedDeviceName));
    state_.emplace(stateFilePath());
}

ActivationController::ActivationController(ActivationConfig config,
                                          std::shared_ptr<moonbase::licensing> licensing,
                                          juce::String deviceName,
                                          std::function<void()> cancelInFlight)
    : config_(std::move(config)), licensing_(std::move(licensing)),
      cancelInFlight_(std::move(cancelInFlight))
{
    jassert(licensing_ != nullptr);
    setDeviceLabel(std::move(deviceName));
    state_.emplace(stateFilePath());
}

std::optional<moonbase::device_id_description> ActivationController::describeDevice() const
{
    if (licensing_ == nullptr)
        return std::nullopt;

    try
    {
        return licensing_->device_resolver().describe_device();
    }
    catch (const std::exception&)
    {
        // A machine with no identity has nothing to describe, and a diagnostics
        // getter is the last place that should throw.
        return std::nullopt;
    }
}

ActivationController::~ActivationController()
{
    stopTimer();
    ++updateGeneration_;     // drop any queued update-flow continuations
    updateDownload_.reset(); // cancels + joins the installer download thread
    // Unblock any in-flight request, then wait for the workers to finish, so no
    // detached thread keeps running module code after we (and possibly the
    // plugin binary) are gone. cancelInFlight_ makes the drain near-instant.
    if (cancelInFlight_)
        cancelInFlight_();
    threadPool_.removeAllJobs(true, 5000);
}

void ActivationController::setDeviceLabel(juce::String deviceName)
{
    deviceLabel_ = deviceName.trim();
    if (deviceLabel_.isEmpty())
        deviceLabel_ = "This device";
    deviceLabel_ << juce::String::fromUTF8("  \xc2\xb7  ") << shortPlatformName(); // middle dot
}

void ActivationController::emitDiagnostic(const juce::String& message)
{
    // Always visible in debug builds; routed to the host's sink when provided.
    // Call only on the message thread (the config callback expects that).
    DBG("[moonbase] " << message);
    if (config_.onDiagnostic)
        config_.onDiagnostic(message);
}

bool ActivationController::ensureReady()
{
    if (licensing_ != nullptr)
        return true;

    const auto reason = configError_.isNotEmpty() ? configError_
                                                  : juce::String("Activation is not configured.");
    emitDiagnostic("Operation ignored: " + reason);
    setScreen(Screen::Error, reason);
    return false;
}

//==============================================================================
void ActivationController::start()
{
    if (! ensureReady())
        return;

    setScreen(Screen::Loading);

    const auto generation = ++generation_;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, licensing]() mutable
    {
        std::optional<moonbase::license> result;
        std::optional<moonbase::license> expiredTrial;
        bool deleteExpiredOffline = false;
        juce::String diag;
        try
        {
            auto stored = licensing->store().load_local_license();
            if (stored)
            {
                // Peek without throwing on a past `exp` so we can tell an expired
                // offline license (permanently dead, never refreshable) apart
                // from an online token (refreshable within its grace period) and
                // from an untrusted token (bad signature/device -> leave it).
                std::optional<moonbase::license> peek;
                try
                {
                    peek = licensing->validate_token_local_allow_expired(stored->token);
                }
                catch (const moonbase::license_device_mismatch_error& ex)
                {
                    // The token is genuine but bound to a different device id.
                    // Its message already distinguishes a stale binding (an older
                    // fingerprint version, which a migrating_device_id_resolver
                    // could accept) from a genuinely foreign machine, so quoting
                    // it beats asserting either.
                    diag = juce::String("Stored token is not bound to this device: ") + ex.what();
                }
                catch (const moonbase::insufficient_device_identity_error& ex)
                {
                    // Nothing was wrong with the token: this machine could not
                    // identify itself, so no comparison was possible. Saying "not
                    // valid for this device" here would send support down entirely
                    // the wrong path.
                    diag = juce::String("Could not identify this device, so the stored license could not be "
                                        "checked: ")
                        + ex.what();
                }
                catch (const std::exception& ex)
                {
                    // Tampered / unparseable -> locked, but left on disk.
                    diag = juce::String("Stored token rejected: ") + ex.what();
                }

                if (peek && peek->method == moonbase::activation_method::offline)
                {
                    const bool expired = peek->expires_at
                        && *peek->expires_at < std::chrono::system_clock::now();
                    if (expired)
                    {
                        deleteExpiredOffline = true; // remove the dead file below
                        diag = "Stored offline license has expired; removing it.";
                    }
                    else
                        result = std::move(peek);
                }
                else if (peek)
                {
                    try
                    {
                        // Validates the local token first (throws immediately if
                        // already expired, with no network call) and otherwise
                        // re-checks against the server when past the throttle.
                        result = licensing->validate_token_online(stored->token);
                    }
                    catch (const moonbase::license_expired_error& ex)
                    {
                        // Expired either locally or per the server's response. A
                        // trial routes to the Expired screen (using the token we
                        // have for display); the plugin stays locked either way.
                        diag = juce::String("Stored license has expired: ") + ex.what();
                        if (peek->trial)
                            expiredTrial = std::move(peek);
                        else
                            result = std::nullopt;
                    }
                    catch (const std::exception& ex)
                    {
                        // Invalid / unreachable-past-grace -> locked.
                        diag = juce::String("Re-validating stored license failed: ") + describeError(ex);
                        result = std::nullopt;
                    }
                }
            }
        }
        catch (const std::exception& ex)
        {
            diag = juce::String("Loading stored license failed: ") + describeError(ex);
            result = std::nullopt;
        }

        juce::MessageManager::callAsync([safe, generation, result, expiredTrial, deleteExpiredOffline, diag]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->generation_.load())
                return;
            if (diag.isNotEmpty())
                self->emitDiagnostic(diag);
            if (deleteExpiredOffline)
                self->deleteStoredLicense();
            if (expiredTrial)
                self->showTrialExpired(std::move(*expiredTrial));
            else
                self->applyLicense(std::move(result));
        });
    });
}

//==============================================================================
void ActivationController::beginOnlineActivation()
{
    if (! ensureReady())
        return;

    setScreen(Screen::BrowserWait);

    const auto generation = ++generation_;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, licensing]() mutable
    {
        std::optional<moonbase::activation_request> request;
        juce::String error;
        try
        {
            request = licensing->request_activation();
        }
        catch (const std::exception& ex)
        {
            error = describeError(ex);
        }

        juce::MessageManager::callAsync([safe, generation, request, error]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->generation_.load())
                return;

            if (! request)
            {
                // Full reason (incl. the entitlement hint) goes to the developer
                // sink; the user sees a friendly, fixed prompt to retry.
                self->emitDiagnostic("request_activation failed: " + error);
                self->setScreen(Screen::Error,
                                "Couldn't reach Moonbase to start activation. "
                                "Check your internet connection and try again.");
                return;
            }

            self->pendingRequest_ = request;
            juce::URL(juce::String(request->browser_url)).launchInDefaultBrowser();
            self->startTimer(kPollIntervalMs);
        });
    });
}

void ActivationController::cancelActivation()
{
    stopTimer();
    pendingRequest_.reset();
    ++generation_;
    pollInFlight_ = false;
    showWelcome();
}

void ActivationController::refreshLicense(bool force, std::function<void(bool)> onComplete)
{
    if (! ensureReady())
    {
        if (onComplete) onComplete(false);
        return;
    }

    // Nothing to refresh, or an offline license (permanent + server-untracked).
    if (! license_ || license_->method == moonbase::activation_method::offline)
    {
        if (license_ && license_->method == moonbase::activation_method::offline)
            emitDiagnostic("refreshLicense: offline licenses are not re-validated online.");
        if (onComplete) onComplete(false);
        return;
    }

    const auto generation = ++generation_;
    const auto token = license_->token;
    const auto currentLicense = *license_; // for the expired-trial case (re-validation can't return it)
    const bool wasTrial = license_->trial;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, token, currentLicense, wasTrial, licensing, force, onComplete]() mutable
    {
        std::optional<moonbase::license> refreshed;
        bool expired = false;
        juce::String diag;
        try
        {
            if (force)
            {
                // Bypass the min-interval throttle by hitting the API directly.
                refreshed = licensing->client().validate_token_online(token);
            }
            else
            {
                // Throttled + grace-period aware (skips the network if recent).
                // Suppress the SDK's own background-thread persist; we persist on
                // the message thread below so a stale write can't resurrect a
                // license the user cleared while this refresh was in flight.
                refreshed = licensing->validate_token_online(token, [] { return false; });
            }
        }
        catch (const moonbase::license_expired_error& ex)
        {
            // Re-validation says it has ended (e.g. a trial that was still valid
            // locally). Distinct from a network blip: this should lock.
            expired = true;
            diag = ex.what();
        }
        catch (const std::exception& ex)
        {
            diag = describeError(ex);
        }

        juce::MessageManager::callAsync([safe, generation, refreshed, expired, currentLicense, wasTrial,
                                         licensing, diag, onComplete]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->generation_.load())
            {
                // Superseded (e.g. deactivate / clearLicense bumped the
                // generation). Drop the result and, crucially, do not persist.
                if (onComplete) onComplete(false);
                return;
            }

            if (refreshed)
            {
                // Persist here on the message thread: serialized against
                // deactivate()/clearLicense() and gated by the generation check
                // above, so a stale refresh cannot recreate a cleared license.
                try
                {
                    auto guard = licensing->store().lock_for_update();
                    licensing->store().store_local_license(*refreshed);
                }
                catch (const moonbase::storage_error& ex)
                {
                    self->emitDiagnostic(juce::String("Refreshed, but couldn't persist the license: ") + ex.what());
                }

                // Updates license_, re-routes the screen if needed, and notifies
                // listeners (onActivationChanged) so the host can reload features.
                self->applyLicense(std::move(refreshed));
                if (onComplete) onComplete(true);
            }
            else if (expired && wasTrial)
            {
                // The trial ended per the server: lock and show the Expired
                // screen (using the trial we held, since the throw returns none).
                self->emitDiagnostic("Trial ended on re-validation: " + diag);
                self->showTrialExpired(currentLicense);
                if (onComplete) onComplete(false);
            }
            else
            {
                // Keep the current license on other failures (a network blip must
                // not lock the user out); just report the underlying reason.
                self->emitDiagnostic("Online re-validation failed: " + diag);
                if (onComplete) onComplete(false);
            }
        });
    });
}

void ActivationController::timerCallback()
{
    if (pollInFlight_ || ! pendingRequest_)
        return;

    pollInFlight_ = true;
    const auto generation = generation_.load();
    const auto request = *pendingRequest_;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, request, licensing]() mutable
    {
        std::optional<moonbase::license> fulfilled;
        bool fatal = false;
        juce::String error;
        juce::String transient;
        try
        {
            fulfilled = licensing->get_requested_activation(request);
        }
        catch (const moonbase::license_invalid_error& ex) { fatal = true; error = ex.what(); }
        catch (const moonbase::license_expired_error& ex) { fatal = true; error = ex.what(); }
        catch (const std::exception& ex)
        {
            // Transient transport/5xx error — keep polling.
            transient = describeError(ex);
        }

        juce::MessageManager::callAsync([safe, generation, fulfilled, fatal, error, transient]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr)
                return;
            self->pollInFlight_ = false;
            if (generation != self->generation_.load())
                return; // cancelled or superseded

            if (fatal)
            {
                self->emitDiagnostic("Activation rejected during polling: " + error);
                self->stopTimer();
                self->pendingRequest_.reset();
                self->setScreen(Screen::Error, "Activation was rejected. " + error);
            }
            else if (fulfilled)
            {
                self->onActivationFulfilled(std::move(*fulfilled));
            }
            else if (transient.isNotEmpty())
            {
                self->emitDiagnostic("Activation poll transient error (still waiting): " + transient);
            }
        });
    });
}

void ActivationController::onActivationFulfilled(moonbase::license value)
{
    stopTimer();
    pendingRequest_.reset();
    ++generation_;

    try
    {
        auto guard = licensing_->store().lock_for_update();
        licensing_->store().store_local_license(value);
    }
    catch (const moonbase::storage_error& ex)
    {
        // Activated but couldn't persist; still unlock for this session.
        emitDiagnostic(juce::String("Activated, but couldn't persist the license: ") + ex.what());
    }

    setLicense(std::move(value));
    setScreen(Screen::Success);
}

//==============================================================================
bool ActivationController::saveOfflineRequest(const juce::File& destination)
{
    if (! ensureReady())
        return false;

    try
    {
        const auto token = licensing_->generate_device_token();
        if (destination.replaceWithText(juce::String(token)))
        {
            offlineRequestSaved_ = true;
            offlineError_.clear();
            sendChangeMessage();
            return true;
        }
        emitDiagnostic("Couldn't write the machine file to " + destination.getFullPathName());
    }
    catch (const std::exception& ex)
    {
        emitDiagnostic(juce::String("Generating the machine file failed: ") + ex.what());
    }

    offlineError_ = "Couldn't write the request file.";
    sendChangeMessage();
    return false;
}

void ActivationController::setOfflineResponse(const juce::File& responseFile)
{
    offlineResponse_ = responseFile;
    offlineError_.clear();
    sendChangeMessage();
}

void ActivationController::activateOffline()
{
    if (! ensureReady())
        return;

    if (offlineResponse_ == juce::File())
    {
        offlineError_ = "Add the response file from moonbase.sh to continue.";
        setScreen(Screen::Offline);
        return;
    }

    const auto contents = offlineResponse_.loadFileAsString();
    ++generation_;

    try
    {
        auto licenseValue = licensing_->read_offline_license(contents.trim().toStdString());
        try
        {
            auto guard = licensing_->store().lock_for_update();
            licensing_->store().store_local_license(licenseValue);
        }
        catch (const moonbase::storage_error& ex)
        {
            emitDiagnostic(juce::String("Activated offline, but couldn't persist the license: ") + ex.what());
        }

        setLicense(std::move(licenseValue));
        offlineError_.clear();
        setScreen(Screen::Success);
    }
    catch (const std::exception& ex)
    {
        emitDiagnostic(juce::String("Offline license file rejected: ") + ex.what());
        offlineError_ = "That response file isn't valid for this device.";
        setScreen(Screen::Offline);
    }
}

//==============================================================================
void ActivationController::deactivate()
{
    if (! ensureReady())
        return;

    if (! license_)
    {
        showWelcome();
        return;
    }

    // Offline / trial licenses can't be revoked server-side — local forget.
    if (license_->method == moonbase::activation_method::offline || license_->trial)
    {
        clearLicense();
        return;
    }

    busy_ = true;
    statusMessage_.clear(); // progress is shown by the inline spinner in the deactivate button
    sendChangeMessage();

    const auto generation = ++generation_;
    const auto token = license_->token;
    const auto activationId = juce::String(license_->activation_id);
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, token, activationId, licensing]() mutable
    {
        enum class Outcome { Revoked, NotRevokable, Unreachable };
        Outcome outcome = Outcome::Revoked;
        juce::String diag;
        try
        {
            licensing->revoke_activation(token);
        }
        catch (const moonbase::operation_not_supported_error& ex) { outcome = Outcome::NotRevokable; diag = ex.what(); }
        catch (const moonbase::license_invalid_error&)            { outcome = Outcome::Revoked; }
        catch (const moonbase::license_expired_error&)            { outcome = Outcome::Revoked; }
        catch (const std::exception& ex)                          { outcome = Outcome::Unreachable; diag = describeError(ex); }

        const int outcomeCode = static_cast<int>(outcome);
        juce::MessageManager::callAsync([safe, generation, outcomeCode, activationId, diag]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->generation_.load())
                return;

            self->busy_ = false;
            switch (static_cast<Outcome>(outcomeCode))
            {
                case Outcome::Revoked:
                    self->deleteStoredMatching(activationId);
                    self->applyLicense(std::nullopt);
                    break;
                case Outcome::NotRevokable:
                    self->clearLicense();
                    break;
                case Outcome::Unreachable:
                    self->emitDiagnostic("revoke_activation couldn't reach Moonbase: " + diag);
                    self->setScreen(Screen::Details,
                                    "Couldn't reach Moonbase to deactivate. Try again when online.");
                    break;
            }
        });
    });
}

void ActivationController::clearLicense()
{
    ++generation_;
    deleteStoredLicense();
    applyLicense(std::nullopt);
}

void ActivationController::deleteStoredLicense()
{
    try
    {
        auto guard = licensing_->store().lock_for_update();
        licensing_->store().delete_local_license();
    }
    catch (const moonbase::storage_error&)
    {
    }
}

void ActivationController::deleteStoredMatching(const juce::String& activationId)
{
    try
    {
        auto guard = licensing_->store().lock_for_update();
        if (auto stored = licensing_->store().load_local_license();
            stored && juce::String(stored->activation_id) == activationId)
        {
            licensing_->store().delete_local_license();
        }
    }
    catch (const moonbase::storage_error&)
    {
    }
}

//==============================================================================
void ActivationController::showWelcome()
{
    offlineResponse_ = juce::File();
    offlineRequestSaved_ = false;
    offlineError_.clear();
    setScreen(Screen::Welcome);
}

void ActivationController::showOffline()
{
    offlineError_.clear();
    setScreen(Screen::Offline);
}

void ActivationController::showDetails()
{
    // For a trial license "details" is the trial view; keep them consistent.
    setScreen(screenForCurrentLicense());
}

void ActivationController::setPreviewState(Screen screen, std::optional<moonbase::license> license,
                                           juce::String previewError, bool busy)
{
    ++generation_; // drop any in-flight start()/async continuation
    stopTimer();
    pollInFlight_ = false;
    busy_ = busy;
    if (screen == Screen::Expired)
    {
        // The Expired screen is locked: keep the license out of license_ so
        // gating stays off; the passed license backs the view's display.
        expiredTrial_ = std::move(license);
        setLicense(std::nullopt);
    }
    else
    {
        setLicense(std::move(license));
        expiredTrial_.reset();
    }
    // Route the preview error to the field the target screen actually shows: the
    // Offline view reads offlineError(); the others (Welcome/Error, Details) read
    // statusMessage().
    if (screen == Screen::Offline)
    {
        offlineError_ = previewError;
        statusMessage_.clear();
    }
    else
    {
        statusMessage_ = previewError;
        offlineError_.clear();
    }
    screen_ = screen;
    // Synchronous so a snapshot harness sees the new screen immediately without
    // pumping the message loop. Must be called on the message thread.
    sendSynchronousChangeMessage();
}

void ActivationController::setPreviewClock(std::optional<std::chrono::system_clock::time_point> now)
{
    previewClock_ = now;
}

//==============================================================================
int ActivationController::trialDaysRemaining() const
{
    if (! license_ || ! license_->expires_at)
        return 0;

    const auto now = previewClock_.value_or(std::chrono::system_clock::now());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             *license_->expires_at - now)
                             .count();
    if (seconds <= 0)
        return 0;
    return static_cast<int>((seconds + 86399) / 86400); // ceil to whole days
}

//==============================================================================
void ActivationController::setScreen(Screen newScreen, const juce::String& message)
{
    screen_ = newScreen;
    statusMessage_ = message;
    sendChangeMessage();
}

void ActivationController::setLicense(std::optional<moonbase::license> value)
{
    license_ = std::move(value);
    // Publish for the audio thread (this always runs on the message thread).
    licensed_.store(license_.has_value(), std::memory_order_release);
}

ActivationController::Screen ActivationController::screenForCurrentLicense() const
{
    if (! license_)
        return Screen::Welcome;
    // A backend-granted trial license shows the trial view (days left / unlock);
    // a full license shows the details view.
    return license_->trial ? Screen::Trial : Screen::Details;
}

void ActivationController::applyLicense(std::optional<moonbase::license> value)
{
    setLicense(std::move(value));
    expiredTrial_.reset();
    statusMessage_.clear();

    // A validated license that outranks the running app routes to the update
    // screen first (unless dismissed this session); "Remind me later" then falls
    // through to the normal Details / Trial screen.
    const auto dest = screenForCurrentLicense();
    if ((dest == Screen::Details || dest == Screen::Trial) && config_.autoPresentUpdate
        && updateAvailable() && ! updateDismissedForCurrentLicense())
    {
        updateFromLicenseView_ = false; // auto-routed, not from the badge
        beginUpdateFlow(dest);
        return;
    }

    setScreen(dest);
}

void ActivationController::showTrialExpired(moonbase::license expired)
{
    // The plugin must stay locked, so license_ stays empty; the ended trial is
    // held separately for the Expired screen to show the product + end date.
    expiredTrial_ = std::move(expired);
    setLicense(std::nullopt);
    statusMessage_.clear();
    setScreen(Screen::Expired);
}

//==============================================================================
// App update flow
bool ActivationController::updateAvailable() const
{
    if (! config_.enableUpdatePrompt || ! license_)
        return false;
    const auto& released = license_->licensed_product.current_release_version;
    if (! released)
        return false;
    return moonbase::update_available(config_.resolvedApplicationVersion().toStdString(), *released);
}

juce::File ActivationController::stateFilePath() const
{
    // A small JSON state file beside the license. Named after the license file so
    // it stays unique to this product/store.
    const auto license = config_.resolvedLicenseFile();
    return license.getSiblingFile(license.getFileNameWithoutExtension() + ".state.json");
}

bool ActivationController::updateDismissedForCurrentLicense() const
{
    if (! state_ || ! license_)
        return false;
    const auto& released = license_->licensed_product.current_release_version;
    if (! released)
        return false;
    return state_->isUpdateIgnored(juce::String(*released));
}

void ActivationController::beginUpdateFlow(Screen /*destination*/)
{
    updateInfo_ = {};
    updateInfo_.phase = UpdateInfo::Phase::Loading;
    updateInfo_.currentVersion = config_.resolvedApplicationVersion();
    if (license_ && license_->licensed_product.current_release_version)
        updateInfo_.newVersion = juce::String(*license_->licensed_product.current_release_version);

    setScreen(Screen::UpdateAvailable);
    fetchUpdateInfo();
}

void ActivationController::fetchUpdateInfo()
{
    if (! inventory_ || ! license_)
    {
        // No way to fetch (test seam): show the screen with whatever we have.
        updateInfo_.phase = UpdateInfo::Phase::Ready;
        sendChangeMessage();
        return;
    }

    const auto generation = ++updateGeneration_;
    juce::WeakReference<ActivationController> safe(this);
    auto inventory = *inventory_;
    auto token = license_->token;
    auto version = updateInfo_.newVersion.toStdString();

    threadPool_.addJob([safe, generation, inventory, token, version]() mutable
    {
        std::optional<moonbase::release_info> info;
        juce::String diag;
        try
        {
            info = inventory.get_release(version, token);
        }
        catch (const std::exception& ex)
        {
            diag = juce::String("fetchUpdateInfo failed: ") + describeError(ex);
        }

        juce::MessageManager::callAsync([safe, generation, info, diag]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->updateGeneration_.load())
                return;
            if (info)
            {
                self->updateInfo_.releaseNotes = juce::String::fromUTF8(info->description.c_str());
                self->updateInfo_.canDownload = self->licenseCanDownload(*info);
                self->updateInfo_.error.clear();
            }
            else
            {
                self->updateInfo_.error = "Couldn't load the release details.";
                self->emitDiagnostic(diag);
            }
            self->updateInfo_.phase = UpdateInfo::Phase::Ready;
            self->sendChangeMessage();
        });
    });
}

bool ActivationController::licenseCanDownload(const moonbase::release_info& info) const
{
    return license_ && moonbase::can_download(*license_, info);
}

void ActivationController::startUpdateDownload()
{
    if (! inventory_ || ! license_ || screen_ != Screen::UpdateAvailable)
        return;
    if (updateInfo_.phase == UpdateInfo::Phase::Downloading)
        return;

    updateInfo_.phase = UpdateInfo::Phase::Downloading;
    updateInfo_.progress = 0.0;
    updateInfo_.error.clear();
    sendChangeMessage();

    const auto generation = ++updateGeneration_;
    juce::WeakReference<ActivationController> safe(this);
    auto inventory = *inventory_;
    auto token = license_->token;
    const auto platformName = moonbase::to_string(moonbase::current_platform());

    threadPool_.addJob([safe, generation, inventory, token, platformName]() mutable
    {
        std::optional<moonbase::download_target> target;
        juce::String diag;
        try
        {
            target = inventory.get_download_url(platformName, token);
        }
        catch (const std::exception& ex)
        {
            diag = juce::String("startUpdateDownload failed: ") + describeError(ex);
        }

        juce::MessageManager::callAsync([safe, generation, target, diag]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->updateGeneration_.load())
                return;
            if (target)
            {
                self->beginFileDownload(*target);
            }
            else
            {
                self->emitDiagnostic(diag);
                self->failUpdateDownload("Couldn't reach Moonbase to download the update. Try again when online.");
            }
        });
    });
}

void ActivationController::beginFileDownload(const moonbase::download_target& target)
{
    auto dir = config_.resolvedDownloadDirectory();
    dir.createDirectory();

    const auto suggested = target.filename.empty()
                               ? defaultInstallerName()
                               : juce::File::createLegalFileName(juce::String(target.filename));
    updateFile_ = dir.getNonexistentChildFile(suggested.upToLastOccurrenceOf(".", false, false),
                                              suggested.fromLastOccurrenceOf(".", true, false));

    downloadGeneration_.store(updateGeneration_.load());
    const auto options = juce::URL::DownloadTaskOptions().withListener(this);
    updateDownload_ = juce::URL(juce::String(target.url)).downloadToFile(updateFile_, options);
    if (updateDownload_ == nullptr)
        failUpdateDownload("Couldn't start the download.");
}

void ActivationController::failUpdateDownload(const juce::String& message)
{
    updateInfo_.phase = UpdateInfo::Phase::Ready;
    updateInfo_.progress = 0.0;
    updateInfo_.error = message;
    sendChangeMessage();
}

juce::String ActivationController::defaultInstallerName() const
{
#if JUCE_MAC
    const char* ext = ".dmg";
#elif JUCE_WINDOWS
    const char* ext = ".exe";
#else
    const char* ext = ".zip";
#endif
    auto base = config_.resolvedProductName();
    if (updateInfo_.newVersion.isNotEmpty())
        base << "-" << updateInfo_.newVersion;
    return juce::File::createLegalFileName(base + ext);
}

void ActivationController::progress(juce::URL::DownloadTask*, juce::int64 bytesDownloaded,
                                    juce::int64 totalLength)
{
    const double frac = totalLength > 0
                            ? juce::jlimit(0.0, 1.0, (double) bytesDownloaded / (double) totalLength)
                            : 0.0;
    const auto generation = downloadGeneration_.load();
    juce::WeakReference<ActivationController> safe(this);
    juce::MessageManager::callAsync([safe, generation, frac]
    {
        auto* self = safe.get();
        if (self == nullptr || generation != self->updateGeneration_.load())
            return;
        if (self->updateInfo_.phase != UpdateInfo::Phase::Downloading)
            return;
        self->updateInfo_.progress = frac;
        self->sendChangeMessage();
    });
}

void ActivationController::finished(juce::URL::DownloadTask*, bool success)
{
    const auto generation = downloadGeneration_.load();
    juce::WeakReference<ActivationController> safe(this);
    juce::MessageManager::callAsync([safe, generation, success]
    {
        auto* self = safe.get();
        if (self == nullptr || generation != self->updateGeneration_.load())
            return;
        if (success)
        {
            self->updateInfo_.phase = UpdateInfo::Phase::Done;
            self->updateInfo_.progress = 1.0;
            self->updateInfo_.error.clear();
            self->updateFile_.revealToUser();
            self->sendChangeMessage();
        }
        else
        {
            self->failUpdateDownload("The download didn't finish. Try again.");
        }
    });
}

void ActivationController::showUpdate(bool fromLicenseView)
{
    if (! updateAvailable())
        return;
    updateFromLicenseView_ = fromLicenseView;
    beginUpdateFlow(screenForCurrentLicense());
}

void ActivationController::revealUpdateDownload()
{
    if (updateFile_ != juce::File() && updateFile_.existsAsFile())
    {
        updateFile_.revealToUser();
        return;
    }

    // The downloaded installer is gone (moved, deleted, or cleaned up). Drop back
    // to the ready state so the button offers to download it again.
    updateFile_ = juce::File();
    updateInfo_.phase = UpdateInfo::Phase::Ready;
    updateInfo_.progress = 0.0;
    sendChangeMessage();
}

void ActivationController::dismissUpdate()
{
    // Remember the dismissed version so we don't prompt for it again next session
    // (a newer release still prompts). Persisted in the JSON state file.
    if (state_)
        state_->ignoreUpdate(updateInfo_.newVersion);

    ++updateGeneration_;     // drop any in-flight fetch / download continuations
    updateDownload_.reset(); // cancel an active file download
    setScreen(screenForCurrentLicense());
}

void ActivationController::setPreviewUpdate(UpdateInfo::Phase phase, moonbase::license license,
                                            juce::String releaseNotes, double progressValue,
                                            juce::String error, bool canDownload)
{
    ++generation_;
    ++updateGeneration_;
    stopTimer();
    pollInFlight_ = false;
    busy_ = false;

    setLicense(std::move(license));
    expiredTrial_.reset();
    statusMessage_.clear();

    updateInfo_ = {};
    updateInfo_.phase = phase;
    updateInfo_.currentVersion = config_.resolvedApplicationVersion();
    if (license_ && license_->licensed_product.current_release_version)
        updateInfo_.newVersion = juce::String(*license_->licensed_product.current_release_version);
    updateInfo_.releaseNotes = std::move(releaseNotes);
    updateInfo_.progress = progressValue;
    updateInfo_.error = std::move(error);
    updateInfo_.canDownload = canDownload;

    screen_ = Screen::UpdateAvailable;
    sendSynchronousChangeMessage();
}

juce::String ActivationController::shortPlatformName()
{
#if JUCE_MAC
    return "macOS";
#elif JUCE_WINDOWS
    return "Windows";
#elif JUCE_LINUX
    return "Linux";
#else
    return juce::SystemStats::getOperatingSystemName();
#endif
}

} // namespace moonbase::juce_integration
