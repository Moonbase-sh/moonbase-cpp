// Implementation of ActivationController. Compiled as part of the single
// moonbase_licensing module translation unit.

#include "ActivationController.h"
#include "juce_http_transport.h"

#include <fstream>

namespace moonbase::juce_integration {

namespace {
constexpr int kPollIntervalMs = 2000;
// How often the license watch looks at the license file and the clock. Both
// checks are local (a stat, a small read when the file changed, and a time
// compare), so this costs next to nothing.
constexpr int kLicenseWatchIntervalMs = 2000;

// Diagnostic-only error text. For transport failures (moonbase::api_error) the
// SDK stashes actionable guidance (e.g. the macOS network entitlement hint) in
// the detail field; append it so it reaches onDiagnostic without ever appearing
// in the friendly, user-facing screen text.
juce::String describeError(const std::exception& ex)
{
    // fromUTF8, because the SDK's messages are UTF-8 (a store path under
    // C:\Users\Björn, a server's own words) and juce::String's const char*
    // constructor reads ASCII: it asserts on anything else and garbles it.
    auto message = juce::String::fromUTF8(ex.what());
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

// What went wrong with a request, as far as the user needs to know. The screen
// copy is chosen from this; the server's own words go to onDiagnostic.
enum class Failure
{
    Unreachable, // no answer from Moonbase (offline, firewall, or something in between answered)
    Busy,        // Moonbase answered but timed out, is rate limiting or failing (408, 429, 5xx): try later
    Refused,     // Moonbase answered and said no; trying again won't change that
    StoreClosed, // the merchant closed their Moonbase account, for good
    NoIdentity,  // this machine has nothing to bind a license to
    Unexpected,  // neither Moonbase's answer nor a transport failure: show what it was
};

Failure classifyFailure(const std::exception& ex)
{
    if (dynamic_cast<const moonbase::insufficient_device_identity_error*>(&ex) != nullptr)
        return Failure::NoIdentity;

    if (dynamic_cast<const moonbase::store_closed_error*>(&ex) != nullptr)
        return Failure::StoreClosed;

    if (const auto* api = dynamic_cast<const moonbase::api_error*>(&ex))
    {
        const auto status = api->status_code();
        if (status == 408 || status == 429 || status >= 500)
            return Failure::Busy;
        // Everything else the SDK left an api_error because it is no verdict: a
        // transport failure (0), a body that was not Moonbase's (2xx, 3xx), or a
        // 4xx that a proxy or misrouted request could have sent as well.
        return Failure::Unreachable;
    }

    // A definitive answer: license_invalid_error and the like.
    if (dynamic_cast<const moonbase::moonbase_error*>(&ex) != nullptr)
        return Failure::Refused;

    // Not from Moonbase, and not the connection either: a transport reports a
    // failed connection as api_error (the http_transport::send contract, which
    // juce_http_transport keeps). So something failed on this machine, such as
    // building the request, and calling that a connection problem sends the
    // user and the developer looking in the wrong place.
    return Failure::Unexpected;
}

// Screen copy for Failure::Unexpected. Nobody anticipated it, so there is no
// friendlier way to put it, and the most useful thing the user can do is pass
// its own words on to the developer.
juce::String unexpectedFailure(const juce::String& lead, const std::exception& ex)
{
    const auto reason = juce::String::fromUTF8(ex.what()).trim();
    return reason.isEmpty() ? lead + "." : lead + ": " + reason;
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

    auto store = std::make_shared<moonbase::file_license_store>(compat::toFilesystemPath(file));
#if JUCE_ANDROID
    // The one thing the core SDK cannot obtain by itself: an application Context.
    // JUCE has one, so hand it over and the core's plain-JNI reader does the rest.
    // Idempotent, so calling it per controller is fine.
    if (auto* env = juce::getEnv())
    {
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) == 0)
        {
            moonbase::android::set_jni_environment(vm, juce::getAppContext().get());

            // The workers are plain threads. JUCE's networking attaches them to
            // the VM, and Android aborts a thread that ends still attached, so
            // detach on the way out, as JUCE's own threads do.
            threadPool_.setWorkerExitHook([vm]
            {
                JNIEnv* attached = nullptr;
                if (vm->GetEnv(reinterpret_cast<void**>(&attached), JNI_VERSION_1_2) == JNI_OK)
                    vm->DetachCurrentThread();
            });
        }
    }
#endif

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
    stopLicenseWatch();
    stopTimer();
    ++updateGeneration_;     // drop any queued update-flow continuations
    updateDownload_.reset(); // cancels + joins the installer download thread
    // Unblock any in-flight request, then wait for the workers to finish, so no
    // detached thread keeps running module code after we (and possibly the
    // plugin binary) are gone. cancelInFlight_ makes the drain near-instant.
    if (cancelInFlight_)
        cancelInFlight_();
    threadPool_.stop();
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

    // Watch the license from here on (see the header). Only a file store has a
    // file another instance can change. The first look is taken before the load,
    // so a change that lands while it runs is still seen.
    if (auto* fileStore = dynamic_cast<moonbase::file_license_store*>(&licensing_->store()))
        watchedFile_ = fileStore->path();
    watchedStamp_.reset();
    if (watchedFile_)
        watchedStamp_ = stampLicenseFile();
    licenseWatch_.onTick = [this] { watchLicense(); };
    licenseWatch_.startTimer(kLicenseWatchIntervalMs);

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
        juce::String userMessage;
        try
        {
            request = licensing->request_activation();
        }
        catch (const std::exception& ex)
        {
            error = describeError(ex);
            switch (classifyFailure(ex))
            {
                case Failure::Unreachable:
                    userMessage = "Couldn't reach Moonbase to start activation. "
                                  "Check your internet connection and try again.";
                    break;
                case Failure::Busy:
                    userMessage = "Moonbase couldn't start activation right now. Try again in a minute.";
                    break;
                case Failure::Refused:
                    userMessage = "Activation isn't available for this product right now. "
                                  "If this keeps happening, contact the developer.";
                    break;
                case Failure::StoreClosed:
                    userMessage = "This store has closed, so activation isn't available.";
                    break;
                case Failure::NoIdentity:
                    userMessage = "This computer can't be identified, so it can't be activated. "
                                  "Contact the developer for help.";
                    break;
                case Failure::Unexpected:
                    userMessage = unexpectedFailure("Activation couldn't start", ex);
                    break;
            }
        }

        juce::MessageManager::callAsync([safe, generation, request, error, userMessage]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->generation_.load())
                return;

            if (! request)
            {
                // Full reason (incl. the entitlement hint) goes to the developer
                // sink; the user sees friendly, fixed copy for the kind of failure,
                // or the reason itself when the failure is of no known kind.
                self->emitDiagnostic("request_activation failed: " + error);
                self->setScreen(Screen::Error, userMessage);
                return;
            }

            self->pendingRequest_ = request;
            const juce::URL link(juce::String(request->browser_url));
            const bool opened = self->config_.openBrowser ? self->config_.openBrowser(link)
                                                          : link.launchInDefaultBrowser();
            if (! opened)
                self->emitDiagnostic("Couldn't open the browser for activation; pendingBrowserUrl() has the link.");
            self->startTimer(kPollIntervalMs);
            self->sendChangeMessage(); // pendingBrowserUrl() is set now
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

juce::String ActivationController::pendingBrowserUrl() const
{
    return pendingRequest_ ? juce::String(pendingRequest_->browser_url) : juce::String();
}

void ActivationController::refreshLicense(bool force, std::function<void(bool)> onComplete)
{
    if (! ensureReady())
    {
        if (onComplete) onComplete(false);
        return;
    }

    // An activation brings a fresh license of its own, and a re-check now would
    // supersede its requests (stranding the flow if the request hasn't arrived).
    if (activationInFlight())
    {
        emitDiagnostic("refreshLicense: skipped while an activation is in progress.");
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

    ++refreshesInFlight_;
    const auto generation = ++generation_;
    const auto token = license_->token;
    const auto currentLicense = *license_; // for the expired-trial case (re-validation can't return it)
    const bool wasTrial = license_->trial;
    const auto gracePeriod = config_.onlineGracePeriod;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, token, currentLicense, wasTrial, gracePeriod, licensing, force,
                        onComplete]() mutable
    {
        std::optional<moonbase::license> refreshed;
        bool expired = false;
        bool rejected = false;
        bool pastGrace = false;
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
            diag = juce::String::fromUTF8(ex.what());
        }
        catch (const moonbase::license_invalid_error& ex)
        {
            // The server refused the license for good: it was revoked or
            // deleted, or the store has closed. Also not a network blip, so this
            // locks too.
            rejected = true;
            diag = juce::String::fromUTF8(ex.what());
        }
        catch (const std::exception& ex)
        {
            diag = describeError(ex);
            // No answer, or none that counts as a verdict. Within the grace
            // period that is a network blip; past it the license has gone
            // unverified for longer than the app allows, which locks, as it
            // does at launch.
            pastGrace = std::chrono::system_clock::now() - currentLicense.validated_at > gracePeriod;
        }

        juce::MessageManager::callAsync([safe, generation, refreshed, expired, rejected, pastGrace,
                                         currentLicense, wasTrial, licensing, diag, onComplete]() mutable
        {
            auto* self = safe.get();
            if (self != nullptr)
                --self->refreshesInFlight_;
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
            else if (expired || rejected || pastGrace)
            {
                // Lock the way start() does for the same answer: drop the license
                // but leave the file, so the next launch checks it again. A full
                // license lands here when its subscription lapsed: the server
                // says LicenseExpired for that, as it does for an ended trial.
                self->emitDiagnostic(pastGrace
                                         ? "Couldn't re-validate within the offline grace period: " + diag
                                         : "License rejected on re-validation: " + diag);
                self->applyLicense(std::nullopt);
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
        juce::String userMessage;
        juce::String transient;
        try
        {
            fulfilled = licensing->get_requested_activation(request);
        }
        catch (const moonbase::activation_request_error& ex)
        {
            // The server answered 400: the request expired or was cancelled, so
            // it can never complete. The server's reason goes to diagnostics.
            fatal = true;
            error = juce::String::fromUTF8(ex.what());
            userMessage = "This activation expired or was cancelled. Activate again to continue.";
        }
        catch (const moonbase::store_closed_error& ex)
        {
            fatal = true;
            error = juce::String::fromUTF8(ex.what());
            userMessage = "This store has closed, so activation isn't available.";
        }
        // The reason the SDK gives is written for developers (a device-binding
        // explanation runs to several sentences), so it goes to diagnostics and
        // the user sees fixed copy that fits the screen.
        catch (const moonbase::license_invalid_error& ex)
        {
            fatal = true;
            error = juce::String::fromUTF8(ex.what());
            userMessage = "Activation was rejected. If this keeps happening, contact the developer.";
        }
        catch (const moonbase::license_expired_error& ex)
        {
            fatal = true;
            error = juce::String::fromUTF8(ex.what());
            userMessage = "This license has expired, so it can't be activated.";
        }
        catch (const std::exception& ex)
        {
            // Transient transport/5xx error — keep polling.
            transient = describeError(ex);
        }

        juce::MessageManager::callAsync([safe, generation, fulfilled, fatal, error, userMessage, transient]() mutable
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
                self->setScreen(Screen::Error, userMessage);
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

    std::string token;
    try
    {
        token = licensing_->generate_device_token();
    }
    catch (const std::exception& ex)
    {
        // Nothing reached the disk, so this is no file problem: say what it was.
        emitDiagnostic("Generating the machine file failed: " + describeError(ex));
        offlineError_ = classifyFailure(ex) == Failure::NoIdentity
                            ? juce::String("This computer can't be identified, so it can't be activated. "
                                           "Contact the developer for help.")
                            : unexpectedFailure("Couldn't create the machine file", ex);
        sendChangeMessage();
        return false;
    }

    if (destination.replaceWithText(juce::String(token)))
    {
        offlineRequestSaved_ = true;
        offlineError_.clear();
        sendChangeMessage();
        return true;
    }

    emitDiagnostic("Couldn't write the machine file to " + destination.getFullPathName());
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
        juce::String userMessage;
        // A license_invalid_error here means the server refused the token for good
        // (it can't be revoked, or the store has closed), so forgetting it locally
        // is all that is left to do.
        try
        {
            licensing->revoke_activation(token);
        }
        catch (const moonbase::operation_not_supported_error& ex) { outcome = Outcome::NotRevokable; diag = juce::String::fromUTF8(ex.what()); }
        catch (const moonbase::license_invalid_error&)            { outcome = Outcome::Revoked; }
        catch (const moonbase::license_expired_error&)            { outcome = Outcome::Revoked; }
        catch (const std::exception& ex)
        {
            outcome = Outcome::Unreachable;
            diag = describeError(ex);
            const auto failure = classifyFailure(ex);
            if (failure == Failure::Busy)
                userMessage = "Moonbase couldn't deactivate right now. Try again in a minute.";
            else if (failure == Failure::Unexpected)
                userMessage = unexpectedFailure("Couldn't deactivate", ex);
            else
                userMessage = "Couldn't reach Moonbase to deactivate. Try again when online.";
        }

        const int outcomeCode = static_cast<int>(outcome);
        juce::MessageManager::callAsync([safe, generation, outcomeCode, activationId, diag, userMessage]() mutable
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
                    self->emitDiagnostic("revoke_activation failed, license kept: " + diag);
                    self->setScreen(Screen::Details, userMessage);
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
// License watch: local checks only (a stat of the license file, a read when it
// changed, and a look at the clock), so no request goes out unless a license has
// actually run out.
void ActivationController::watchLicense()
{
    // Nothing is settled while the stored license is still loading.
    if (screen_ == Screen::Loading)
        return;

    checkLicenseFile();
    checkLicenseDeadlines();
}

void ActivationController::stopLicenseWatch()
{
    licenseWatch_.stopTimer();
}

ActivationController::FileStamp ActivationController::stampLicenseFile() const
{
    std::error_code ec;
    FileStamp stamp;
    stamp.exists = std::filesystem::is_regular_file(*watchedFile_, ec);
    if (stamp.exists)
    {
        stamp.size = std::filesystem::file_size(*watchedFile_, ec);
        stamp.modified = std::filesystem::last_write_time(*watchedFile_, ec);
    }
    return stamp;
}

void ActivationController::checkLicenseFile()
{
    if (! watchedFile_)
        return;

    const auto stamp = stampLicenseFile();
    if (watchedStamp_ && *watchedStamp_ == stamp)
        return;

    // Only a file that was there and went away means another instance removed
    // it. One that never got written (a failed save) must not lock the license
    // this instance holds in memory.
    const bool existed = watchedStamp_ && watchedStamp_->exists;
    watchedStamp_ = stamp;

    if (! stamp.exists)
    {
        if (existed)
            onLicenseFileRemoved();
        return;
    }

    // Read the file itself rather than through the store. The store lock can be
    // held by another process for the length of a network check, and
    // load_local_license() deletes a file it can't parse, which is exactly what
    // a file caught mid-write looks like. A torn read just fails to parse; the
    // write that completes it changes the stamp, so the next tick reads it again.
    std::optional<moonbase::license> onDisk;
    try
    {
        std::ifstream in(*watchedFile_);
        onDisk = nlohmann::json::parse(in).get<moonbase::license>();
    }
    catch (const std::exception&)
    {
        return;
    }

    if (license_ && onDisk->token == license_->token)
        return; // our own write, or a sibling's that changed nothing

    std::optional<moonbase::license> stored;
    try
    {
        stored = licensing_->validate_token_local(onDisk->token);
    }
    catch (const std::exception&)
    {
        // Expired, bound to another device, or tampered with: nothing this
        // instance can use. Its own checks decide about its own license.
        return;
    }

    // Signed and unexpired, but unverified for longer than the grace period:
    // start() would only take it after an online check, so don't unlock on it
    // here either. Whoever re-validates it writes a fresh copy.
    if (stored->method != moonbase::activation_method::offline
        && std::chrono::system_clock::now() - stored->validated_at > config_.onlineGracePeriod)
        return;

    adoptStoredLicense(std::move(*stored));
}

void ActivationController::onLicenseFileRemoved()
{
    // Another instance or process deactivated or forgot this machine's license,
    // so this one locks too.
    if (! license_)
        return;

    if (activationInFlight())
    {
        // The user is activating here (typically unlocking a trial): lock, but
        // leave the flow on screen and running, as an expiry does. When it
        // lands, it brings a license of its own.
        emitDiagnostic("The license file was removed by another instance or process; locking. "
                       "The activation in progress carries on.");
        setLicense(std::nullopt);
        sendChangeMessage();
        return;
    }

    emitDiagnostic("The license file was removed by another instance or process; locking.");
    endActivationFlows(); // drop in-flight work for the removed license (a refresh, a deactivation)
    applyLicense(std::nullopt);
}

void ActivationController::adoptStoredLicense(moonbase::license stored)
{
    // Two processes' refreshes can land on disk out of order; never trade the
    // copy we hold for an older one of the same activation.
    if (license_ && license_->activation_id == stored.activation_id
        && stored.validated_at < license_->validated_at)
        return;

    if (license_ && license_->activation_id == stored.activation_id)
    {
        // A newer copy of the activation we hold, typically a sibling's
        // re-validation: take it without moving the screen, so an open flow or
        // an update download is never interrupted.
        setLicense(std::move(stored));
        sendChangeMessage();
        return;
    }

    // A different activation: activated or replaced elsewhere. Anything still
    // in flight here belongs to the old one, and must not land on top of the
    // new license (a refresh of a revoked activation would lock it, or write the
    // old token back over the new file), so drop it all and show the license.
    emitDiagnostic("Picked up a license stored by another instance or process.");
    endActivationFlows();
    applyLicense(std::move(stored));
}

bool ActivationController::activationInFlight() const noexcept
{
    // From the click (the request is still being created) until it completes or
    // is cancelled.
    return pendingRequest_.has_value() || screen_ == Screen::BrowserWait;
}

void ActivationController::checkLicenseDeadlines()
{
    // Not while a refresh is already on its way (the host's own, after a
    // purchase, or an earlier one of ours): starting another would supersede it.
    if (! license_ || busy_ || refreshesInFlight_ > 0)
        return;

    const auto now = std::chrono::system_clock::now();
    const bool expired = license_->expires_at && *license_->expires_at <= now;
    const bool offline = license_->method == moonbase::activation_method::offline;
    const bool pastGrace = ! offline && now - license_->validated_at > config_.onlineGracePeriod;
    if (! expired && ! pastGrace)
        return;

    if (activationInFlight())
    {
        // Typically a trial being unlocked. A re-check would supersede the
        // activation's own requests, so stop honouring the license right here
        // and leave the flow alone: the activation replaces it when it lands,
        // and cancelling it returns to the welcome screen.
        emitDiagnostic("The license ran out while an activation was in progress; locking.");
        if (license_->trial)
            expiredTrial_ = license_;
        setLicense(std::nullopt);
        sendChangeMessage();
        return;
    }

    if (offline)
    {
        // Offline licenses are never re-validated, and one past its `exp` is
        // dead for good: lock and remove it, as start() does.
        emitDiagnostic("The offline license has expired; removing it.");
        ++generation_;
        deleteStoredLicense();
        applyLicense(std::nullopt);
        return;
    }

    if (! expired)
    {
        const auto steadyNow = std::chrono::steady_clock::now();
        if (lastGraceCheck_ && steadyNow - *lastGraceCheck_ < config_.onlineCheckInterval)
            return;
        lastGraceCheck_ = steadyNow;
    }

    // Re-check it. A passed `exp` locks with no network call (a trial routes to
    // the Expired screen); past the grace period the server gets one more
    // chance, and the license locks if it can't be reached.
    refreshLicense(false);
}

void ActivationController::endActivationFlows()
{
    stopTimer();
    pendingRequest_.reset();
    pollInFlight_ = false;
    ++generation_;   // in-flight continuations no-op from here on...
    busy_ = false;   // ...including a deactivation's, which would have cleared this
    offlineResponse_ = juce::File();
    offlineRequestSaved_ = false;
    offlineError_.clear();
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
    stopLicenseWatch(); // a preview stays exactly as set
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
    notifyLicenseChange();
}

void ActivationController::notifyLicenseChange()
{
    // Deliver after the current call has finished updating the screen and
    // status, so the host reads a consistent controller. Several changes in one
    // go collapse into one delivery, which reports only if the end result moved.
    if (std::exchange(licenseChangePending_, true))
        return;

    juce::WeakReference<ActivationController> safe(this);
    juce::MessageManager::callAsync([safe]
    {
        if (auto* self = safe.get())
            self->deliverLicenseChange();
    });
}

void ActivationController::deliverLicenseChange()
{
    licenseChangePending_ = false;

    auto token = license_ ? std::optional<std::string>(license_->token) : std::nullopt;
    if (licenseReported_ && token == reportedToken_)
        return;

    licenseReported_ = true;
    reportedToken_ = std::move(token);
    if (onLicenseChanged)
        onLicenseChanged(license_.has_value());
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
    // screen first (unless that version was skipped); "Skip this update" then
    // falls through to the normal Details / Trial screen.
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
        juce::String userMessage;
        try
        {
            target = inventory.get_download_url(platformName, token);
        }
        catch (const std::exception& ex)
        {
            diag = juce::String("startUpdateDownload failed: ") + describeError(ex);
            const auto* api = dynamic_cast<const moonbase::api_error*>(&ex);
            const auto status = api != nullptr ? api->status_code() : 0;
            if (classifyFailure(ex) == Failure::StoreClosed)
                userMessage = "This store has closed, so updates can't be downloaded.";
            else if (status == 403)
                userMessage = "This license can't download this update.";
            else if (status == 404)
                userMessage = "There's no download of this update for your system yet.";
            else if (classifyFailure(ex) == Failure::Busy)
                userMessage = "Moonbase couldn't prepare the download right now. Try again in a minute.";
            else if (classifyFailure(ex) == Failure::Unexpected)
                userMessage = unexpectedFailure("Couldn't start the download", ex);
            else
                userMessage = "Couldn't reach Moonbase to download the update. Try again when online.";
        }

        juce::MessageManager::callAsync([safe, generation, target, diag, userMessage]() mutable
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
                self->failUpdateDownload(userMessage);
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
    // Remember the skipped version so we don't prompt for it again, in this
    // session or the next (a newer release still prompts). Persisted in the JSON
    // state file.
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
    stopLicenseWatch(); // a preview stays exactly as set
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
