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

// Screen copy for an add-on request that got no answer about the add-on: the
// connection, Moonbase, or this machine. `lead` says what failed ("Couldn't
// activate Drum Kit"), and `notFound` what a 404 means for this request.
juce::String addOnFailure(const std::exception& ex, const juce::String& lead, const juce::String& notFound)
{
    if (const auto* api = dynamic_cast<const moonbase::api_error*>(&ex); api != nullptr && api->status_code() == 404)
        return notFound;

    switch (classifyFailure(ex))
    {
        case Failure::Unreachable: return "Couldn't reach Moonbase. Check your internet connection and try again.";
        case Failure::Busy:        return "Moonbase couldn't do that right now. Try again in a minute.";
        case Failure::StoreClosed: return "This store has closed, so add-ons aren't available.";
        case Failure::NoIdentity:  return "This computer can't be identified. Contact the developer for help.";
        case Failure::Refused:     return lead + ". If this keeps happening, contact the developer.";
        case Failure::Unexpected:  break;
    }
    return unexpectedFailure(lead, ex);
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
            config_.toLicensingOptions(), store, deviceIds, transport);
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

    cancelInFlight_ = [transport, inventoryTransport, store]
    {
        transport->cancel();
        inventoryTransport->cancel();
        // A worker waiting its turn on the license lock while another instance
        // re-validates stops waiting, rather than hold up the host until that
        // check is done.
        store->cancel_lock_waits();
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
    // Unblock any in-flight request or wait for the license lock, then wait for
    // the workers to finish, so no detached thread keeps running module code
    // after we (and possibly the plugin binary) are gone. cancelInFlight_ makes
    // the drain near-instant.
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

    lockReason_ = LockReason::None;
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
        std::optional<NotCoveredInfo> notCovered;
        bool deleteExpiredOffline = false;
        auto reason = LockReason::None; // why a stored license didn't load
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
                    reason = LockReason::Invalid;
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
                    reason = LockReason::Invalid;
                }

                if (peek && peek->method == moonbase::activation_method::offline)
                {
                    const bool expired = peek->expires_at
                        && *peek->expires_at < std::chrono::system_clock::now();
                    if (expired)
                    {
                        deleteExpiredOffline = true; // remove the dead file below
                        diag = "Stored offline license has expired; removing it.";
                        reason = LockReason::Expired;
                    }
                    else
                    {
                        try
                        {
                            licensing->validator().ensure_covers_application_version(*peek);
                            result = std::move(peek);
                        }
                        catch (const moonbase::release_not_covered_error& ex)
                        {
                            // Kept on disk: it still covers the versions it did.
                            diag = juce::String("Stored offline license doesn't cover this version: ") + ex.what();
                            notCovered = notCoveredFrom(ex, *peek);
                        }
                    }
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
                        reason = LockReason::Expired;
                        if (peek->trial)
                            expiredTrial = std::move(peek);
                        else
                            result = std::nullopt;
                    }
                    catch (const moonbase::release_not_covered_error& ex)
                    {
                        // Good, but not for this version -> locked, and kept on
                        // disk (the SDK already stored the copy Moonbase sent).
                        diag = juce::String("Stored license doesn't cover this version: ") + ex.what();
                        notCovered = notCoveredFrom(ex, *peek);
                    }
                    catch (const moonbase::license_invalid_error& ex)
                    {
                        // Refused for good, such as revoked -> locked.
                        diag = juce::String("Re-validating stored license failed: ") + describeError(ex);
                        reason = LockReason::Invalid;
                    }
                    catch (const moonbase::storage_error& ex)
                    {
                        // Our own file lock failed; nothing to tell the customer.
                        diag = juce::String("Re-validating stored license failed: ") + describeError(ex);
                    }
                    catch (const std::exception& ex)
                    {
                        // Unreachable past the grace period -> locked.
                        diag = juce::String("Re-validating stored license failed: ") + describeError(ex);
                        reason = LockReason::Unverified;
                    }
                }
            }
        }
        catch (const std::exception& ex)
        {
            diag = juce::String("Loading stored license failed: ") + describeError(ex);
            result = std::nullopt;
        }

        juce::MessageManager::callAsync([safe, generation, result, expiredTrial, notCovered, deleteExpiredOffline,
                                         reason, diag]() mutable
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
            else if (notCovered)
                self->showNotCovered(std::move(*notCovered));
            else if (result)
                self->applyLicense(std::move(result));
            else
                self->lock(reason);
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
            // When it can't be opened, the screen offers the link to copy.
            const bool opened = self->openBrowser(link);
            self->browserOpenFailed_ = ! opened;
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
    // Started from the NotCovered screen to upgrade: back there, not to Welcome.
    if (notCovered_ && ! license_)
        setScreen(Screen::NotCovered);
    else
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

    // A deactivation on its way settles the license either way, and a re-check
    // now would supersede it and leave it unfinished.
    if (busy_)
    {
        emitDiagnostic("refreshLicense: skipped while a deactivation is in progress.");
        if (onComplete) onComplete(false);
        return;
    }

    // On the NotCovered screen, the license that doesn't cover this version is
    // the one to re-check: an upgrade bought since then makes it cover it.
    const auto* current = license_ ? &*license_ : notCovered_ ? &notCovered_->license : nullptr;

    // Nothing to refresh, or an offline license (permanent + server-untracked).
    if (current == nullptr || current->method == moonbase::activation_method::offline)
    {
        if (current != nullptr)
            emitDiagnostic("refreshLicense: offline licenses are not re-validated online.");
        if (onComplete) onComplete(false);
        return;
    }

    ++refreshesInFlight_;
    const auto generation = ++generation_;
    const auto token = current->token;
    const auto currentLicense = *current; // for the expired-trial case (re-validation can't return it)
    const bool wasTrial = current->trial;
    if (screen_ == Screen::NotCovered)
        sendChangeMessage(); // isRefreshing() is set now
    const auto gracePeriod = config_.onlineGracePeriod;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, token, currentLicense, wasTrial, gracePeriod, licensing, force,
                        onComplete, onlineCalls = onlineCalls_, generations = generations_]() mutable
    {
        // Takes turns with the add-on calls until its answer is queued (see
        // onlineCalls_).
        std::lock_guard<std::mutex> oneAtATime(*onlineCalls);

        // Asked under the store's lock, just before the answer is stored: not
        // when this check was superseded (deactivate and clearLicense among
        // others move the generation on first, and delete under the same lock
        // after), and not over another activation stored meanwhile. With nothing
        // stored it is written, which also puts back a license whose first save
        // failed.
        const auto stillCurrent = [licensing, generations, generation, activationId = currentLicense.activation_id]
        {
            if (generations->load() != generation)
                return false;
            const auto stored = licensing->store().load_local_license();
            return ! stored.has_value() || stored->activation_id == activationId;
        };

        std::optional<moonbase::license> refreshed;
        std::optional<NotCoveredInfo> notCovered;
        bool expired = false;
        bool rejected = false;
        bool pastGrace = false;
        juce::String diag;
        try
        {
            if (force)
            {
                // Past the min-interval throttle, straight to the API, but holding
                // the store's lock from the request until the answer is stored, as
                // licensing does for its own checks: answers then reach the store
                // in the order the server gave them, whichever instance asked.
                auto guard = licensing->store().lock_for_update();
                try
                {
                    refreshed = licensing->client().validate_token_online(token);
                }
                catch (const moonbase::release_not_covered_error& ex)
                {
                    // The copy Moonbase sent with the refusal still covers the
                    // versions it did, so it is stored the same way.
                    if (ex.license() && stillCurrent())
                    {
                        try
                        {
                            licensing->store().store_local_license(*ex.license());
                        }
                        catch (const moonbase::storage_error&)
                        {
                        }
                    }
                    throw;
                }
                if (stillCurrent())
                {
                    try
                    {
                        licensing->store().store_local_license(*refreshed);
                    }
                    catch (const moonbase::storage_error& ex)
                    {
                        diag = juce::String("Refreshed, but couldn't persist the license: ") + ex.what();
                    }
                }
            }
            else
            {
                // Throttled + grace-period aware (skips the network if recent),
                // and stores what it got the same way.
                refreshed = licensing->validate_token_online(token, stillCurrent);
            }
        }
        catch (const moonbase::license_expired_error& ex)
        {
            // Re-validation says it has ended (e.g. a trial that was still valid
            // locally). Distinct from a network blip: this should lock.
            expired = true;
            diag = juce::String::fromUTF8(ex.what());
        }
        catch (const moonbase::release_not_covered_error& ex)
        {
            // Good, but not for this version: this locks, but keeps the license.
            notCovered = notCoveredFrom(ex, currentLicense);
            diag = juce::String::fromUTF8(ex.what());
        }
        catch (const moonbase::license_invalid_error& ex)
        {
            // The server refused the license for good: it was revoked or
            // deleted. Also not a network blip, so this locks too.
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

        juce::MessageManager::callAsync([safe, generation, refreshed, notCovered, expired, rejected, pastGrace,
                                         currentLicense, wasTrial, licensing, force, diag, onComplete]() mutable
        {
            auto* self = safe.get();
            if (self != nullptr)
                --self->refreshesInFlight_;
            if (self == nullptr || generation != self->generation_.load())
            {
                // Superseded (e.g. deactivate / clearLicense bumped the
                // generation). Drop the result and, crucially, do not persist.
                if (self != nullptr && self->screen_ == Screen::NotCovered)
                    self->sendChangeMessage(); // isRefreshing() may have cleared
                if (onComplete) onComplete(false);
                return;
            }

            // A throttled check can answer with the very license it was asked
            // about. When the license held here has moved on since (an add-on call
            // landed, say), that answer is the older of the two: keep what is held.
            // A forced check always reached the server, after anything held here,
            // so its answer applies even when it matches the license it was asked
            // about: an add-on revoked since then is gone again.
            if (! force && refreshed && refreshed->token == currentLicense.token && self->license_
                && self->license_->activation_id == refreshed->activation_id
                && self->license_->token != currentLicense.token)
            {
                if (onComplete) onComplete(true);
                return;
            }

            if (refreshed)
            {
                // Stored already, on the worker, if it was still current then.
                if (diag.isNotEmpty())
                    self->emitDiagnostic(diag);

                // Updates license_, re-routes the screen if needed, and notifies
                // listeners (onActivationChanged) so the host can reload features.
                self->applyLicense(std::move(refreshed));
                if (onComplete) onComplete(true);
            }
            else if (notCovered && self->isOlderThanHeld(notCovered->license))
            {
                // A newer copy arrived while this answer was on its way, such as
                // a sibling's after the upgrade, and outranks it.
                self->emitDiagnostic("Dropped a re-validation answer older than the license held: " + diag);
                if (onComplete) onComplete(false);
            }
            else if (notCovered)
            {
                // The copy Moonbase sent along was stored on the worker, as a
                // refreshed license is.
                self->emitDiagnostic("License doesn't cover this version on re-validation: " + diag);
                self->showNotCovered(std::move(*notCovered));
                if (onComplete) onComplete(false);
            }
            else if (expired && wasTrial)
            {
                // The trial ended per the server: lock and show the Expired
                // screen (using the trial we held, since the throw returns none).
                self->emitDiagnostic("Trial ended on re-validation: " + diag);
                self->showTrialExpired(currentLicense);
                if (onComplete) onComplete(false);
            }
            else if (! expired && ! rejected && self->notCovered_ && ! self->license_)
            {
                // "Check again" on the NotCovered screen got no answer. The
                // plugin is locked already, so it just says so and stays put.
                self->emitDiagnostic("Couldn't re-check the license that doesn't cover this version: " + diag);
                self->setScreen(Screen::NotCovered,
                                "Couldn't reach Moonbase to check your license. Try again when you're online.");
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
                self->lock(expired ? LockReason::Expired
                                   : rejected ? LockReason::Invalid : LockReason::Unverified);
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

    // Activated but couldn't persist; still unlock for this session.
    storeLicense(value, "Activated");

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
        storeLicense(licenseValue, "Activated offline");

        setLicense(std::move(licenseValue));
        offlineError_.clear();
        setScreen(Screen::Success);
    }
    catch (const moonbase::release_not_covered_error& ex)
    {
        // This computer's license, for the versions it covers: kept, but locked.
        emitDiagnostic(juce::String("Offline license doesn't cover this version: ") + ex.what());
        offlineError_.clear();
        if (ex.license())
        {
            storeLicense(*ex.license(), "Activated offline");
            showNotCovered(notCoveredFrom(ex, *ex.license()));
        }
        else
        {
            offlineError_ = "That license doesn't cover this version.";
            setScreen(Screen::Offline);
        }
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

    // A license that doesn't cover this version still holds this computer's
    // seat, which deactivating hands back.
    const auto* current = license_ ? &*license_ : notCovered_ ? &notCovered_->license : nullptr;
    if (current == nullptr)
    {
        showWelcome();
        return;
    }

    // Offline / trial licenses can't be revoked server-side — local forget.
    if (current->method == moonbase::activation_method::offline || current->trial)
    {
        clearLicense();
        return;
    }

    busy_ = true;
    statusMessage_.clear(); // progress is shown by the inline spinner in the deactivate button
    sendChangeMessage();

    const auto generation = ++generation_;
    const auto token = current->token;
    const auto activationId = juce::String(current->activation_id);
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, token, activationId, licensing]() mutable
    {
        enum class Outcome { Revoked, NotRevokable, Unreachable };
        Outcome outcome = Outcome::Revoked;
        juce::String diag;
        juce::String userMessage;
        // A license_invalid_error here means the server refused the token for good
        // (it can't be revoked), so forgetting it locally is all that is left to
        // do. On a closed store the license stays: this device could never be
        // activated again.
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
            if (failure == Failure::StoreClosed)
                userMessage = "This store has closed, so this computer can't be deactivated.";
            else if (failure == Failure::Busy)
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
                    self->lock(LockReason::Deactivated);
                    break;
                case Outcome::NotRevokable:
                    self->clearLicense();
                    break;
                case Outcome::Unreachable:
                    self->emitDiagnostic("revoke_activation failed, license kept: " + diag);
                    self->setScreen(self->screenForCurrentLicense(), userMessage);
                    break;
            }
        });
    });
}

void ActivationController::clearLicense()
{
    ++generation_;
    deleteStoredLicense();
    lock(LockReason::Deactivated);
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
    checkPurchaseReturn();
    checkCountdowns();
}

// The screens count down to when the license and its add-ons end ("5 days
// left"), and nothing reports a day going by: broadcast a change when one of
// those counts moves, so a screen left open keeps up. Not when what ends has
// changed: whatever changed it broadcast already.
void ActivationController::checkCountdowns()
{
    std::vector<std::chrono::system_clock::time_point> ends;
    const auto add = [&ends](const std::optional<std::chrono::system_clock::time_point>& end)
    {
        if (end)
            ends.push_back(*end);
    };
    if (license_)
    {
        add(license_->expires_at);
        for (const auto& entry : license_->sub_products)
            add(entry.expires_at);
    }
    for (const auto& entry : addOns_.addOns)
    {
        add(entry.expires_at);
        if (entry.trial)
            add(entry.trial->expires_at);
    }

    std::vector<int> days;
    for (const auto& end : ends)
        days.push_back(daysUntil(end));

    const bool dayWentBy = ends == countdownEnds_ && days != countdownDays_;
    countdownEnds_ = std::move(ends);
    countdownDays_ = std::move(days);
    if (dayWentBy)
        sendChangeMessage();
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

    // Our own write, or a sibling's that changed nothing.
    if ((license_ && onDisk->token == license_->token)
        || (notCovered_ && onDisk->token == notCovered_->license.token))
        return;

    std::optional<moonbase::license> stored;
    std::optional<NotCoveredInfo> notCovered;
    try
    {
        stored = licensing_->validate_token_local(onDisk->token);
    }
    catch (const moonbase::release_not_covered_error& ex)
    {
        // Good, but not for this version: typically a sibling's answer from
        // Moonbase, stored along with the refusal. Taken like any stored
        // license, except that it locks.
        if (! ex.license())
            return;
        notCovered = notCoveredFrom(ex, *ex.license());
        stored = notCovered->license;
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

    if (notCovered)
        adoptNotCovered(std::move(*notCovered));
    else
        adoptStoredLicense(std::move(*stored));
}

void ActivationController::onLicenseFileRemoved()
{
    // Another instance or process deactivated or forgot this machine's license,
    // so this one locks too.
    if (! license_)
    {
        // The license that didn't cover this version is gone as well.
        if (! notCovered_)
            return;
        emitDiagnostic("The license file was removed by another instance or process.");
        if (activationInFlight())
        {
            // As below: the activation in progress carries on and brings a
            // license of its own.
            notCovered_.reset();
            lockReason_ = LockReason::Deactivated;
            sendChangeMessage();
            return;
        }
        endActivationFlows(); // a "Check again" in flight must not write it back
        lock(LockReason::Deactivated);
        return;
    }

    if (activationInFlight())
    {
        // The user is activating here (typically unlocking a trial): lock, but
        // leave the flow on screen and running, as an expiry does. When it
        // lands, it brings a license of its own.
        emitDiagnostic("The license file was removed by another instance or process; locking. "
                       "The activation in progress carries on.");
        lockReason_ = LockReason::Deactivated;
        setLicense(std::nullopt);
        sendChangeMessage();
        return;
    }

    emitDiagnostic("The license file was removed by another instance or process; locking.");
    endActivationFlows(); // drop in-flight work for the removed license (a refresh, a deactivation)
    lock(LockReason::Deactivated);
}

bool ActivationController::isOlderThanHeld(const moonbase::license& copy) const
{
    const auto* held = license_ ? &*license_ : notCovered_ ? &notCovered_->license : nullptr;
    return held != nullptr && held->activation_id == copy.activation_id && copy.validated_at < held->validated_at;
}

void ActivationController::adoptStoredLicense(moonbase::license stored)
{
    // Two processes' refreshes can land on disk out of order; never trade the
    // copy we hold for an older one of the same activation. That includes one
    // Moonbase said doesn't cover this version, which only a newer copy undoes.
    if (isOlderThanHeld(stored))
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

void ActivationController::adoptNotCovered(NotCoveredInfo info)
{
    // As adoptStoredLicense: never an older copy of the activation held here.
    // An activation in progress brings a license of its own.
    if (isOlderThanHeld(info.license) || activationInFlight())
        return;

    const auto* held = license_ ? &*license_ : notCovered_ ? &notCovered_->license : nullptr;
    const bool sameActivation = held != nullptr && held->activation_id == info.license.activation_id;
    emitDiagnostic("Another instance or process found that the license doesn't cover this version; locking.");

    // The file holds no upgrade link, so keep the one Moonbase gave here.
    if (sameActivation && notCovered_ && info.upgradeUrl.isEmpty())
        info.upgradeUrl = notCovered_->upgradeUrl;

    // A different activation's in-flight work must not land on top of it.
    if (! sameActivation)
        endActivationFlows();
    showNotCovered(std::move(info));
}

bool ActivationController::activationInFlight() const noexcept
{
    // From the click (the request is still being created) until it completes or
    // is cancelled.
    return pendingRequest_.has_value() || screen_ == Screen::BrowserWait;
}

void ActivationController::checkLicenseDeadlines()
{
    // An add-on's trial (or subscription) can run out while the license itself
    // carries on, which changes what the host may unlock without a new token.
    publishSubProductFlags();
    if (license_ && licenseReported_)
    {
        const auto active = activeSubProducts();
        if (active != reportedSubProducts_)
        {
            notifyLicenseChange();
            sendChangeMessage();
        }
        for (const auto& id : active)
            lapsesChecked_.removeString(id); // unlocked again: may be checked again later

        // One whose time ran out may have been bought or renewed meanwhile, which
        // the server turns into a seat at the next check: check now, once for
        // each, as an expiring license is checked. Looked for on every tick, so a
        // check put off (by a deactivation on its way, say) still happens.
        if (license_->method == moonbase::activation_method::online && ! busy_ && refreshesInFlight_ == 0
            && ! activationInFlight())
        {
            juce::StringArray ranOut;
            const auto now = clockNow();
            for (const auto& entry : license_->sub_products)
            {
                const auto id = juce::String::fromUTF8(entry.id.c_str());
                if (entry.active && entry.expires_at && *entry.expires_at <= now && ! lapsesChecked_.contains(id))
                    ranOut.add(id);
            }
            if (! ranOut.isEmpty())
            {
                lapsesChecked_.addArray(ranOut);
                emitDiagnostic("An add-on ran out; re-checking the license in case it was bought or renewed.");
                refreshLicense(true);
                return;
            }
        }
    }

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
        lockReason_ = expired ? LockReason::Expired : LockReason::Unverified;
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
        lock(LockReason::Expired);
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
    // The add-ons were listed for the license being replaced; whatever replaces
    // it routes the screen.
    resetAddOns();
    if (screen_ == Screen::AddOns)
        screen_ = Screen::Details;
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

void ActivationController::showAddOns()
{
    // Add-ons are activated and tried from a full license.
    if (! license_ || license_->trial)
        return;

    addOns_.actionError.clear();
    setScreen(Screen::AddOns);
    loadAddOns();
}

void ActivationController::setPreviewState(Screen screen, std::optional<moonbase::license> license,
                                           juce::String previewError, bool busy, LockReason lockReason)
{
    ++generation_; // drop any in-flight start()/async continuation
    stopTimer();
    stopLicenseWatch(); // a preview stays exactly as set
    pollInFlight_ = false;
    busy_ = busy;
    notCovered_.reset();
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
    lockReason_ = lockReason;
    pendingRequest_.reset();
    resetAddOns();
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

void ActivationController::setPreviewBrowserLink(juce::String browserUrl, bool opened)
{
    moonbase::activation_request request;
    request.id = "preview";
    request.browser_url = browserUrl.toStdString();
    pendingRequest_ = std::move(request);
    browserOpenFailed_ = ! opened;
    sendSynchronousChangeMessage();
}

void ActivationController::setPreviewClock(std::optional<std::chrono::system_clock::time_point> now)
{
    previewClock_ = now;
}

std::chrono::system_clock::time_point ActivationController::clockNow() const
{
    return previewClock_.value_or(std::chrono::system_clock::now());
}

void ActivationController::setPreviewAddOns(moonbase::license license, AddOnsInfo info)
{
    ++generation_;
    resetAddOns();
    stopTimer();
    stopLicenseWatch(); // a preview stays exactly as set
    pollInFlight_ = false;
    busy_ = false;
    pendingRequest_.reset();

    setLicense(std::move(license));
    expiredTrial_.reset();
    statusMessage_.clear();
    addOns_ = std::move(info);

    screen_ = Screen::AddOns;
    sendSynchronousChangeMessage();
}

//==============================================================================
// Add-ons
bool ActivationController::hasSubProduct(const juce::String& subProductId) const
{
    JUCE_ASSERT_MESSAGE_THREAD // keep an atomic flag for the audio thread (see the header)
    return license_ && moonbase::has_sub_product(*license_, subProductId.toStdString(), clockNow());
}

const std::atomic<bool>* ActivationController::subProductFlag(const juce::String& subProductId) const
{
    JUCE_ASSERT_MESSAGE_THREAD // once, ahead of the audio thread (see the header)
    auto [entry, added] = subProductFlags_.try_emplace(subProductId);
    if (added)
        entry->second.store(hasSubProduct(subProductId), std::memory_order_release);
    return &entry->second;
}

void ActivationController::publishSubProductFlags()
{
    for (auto& [id, flag] : subProductFlags_)
        flag.store(hasSubProduct(id), std::memory_order_release);
}

juce::StringArray ActivationController::activeSubProducts() const
{
    JUCE_ASSERT_MESSAGE_THREAD
    juce::StringArray ids;
    if (! license_)
        return ids;

    const auto now = clockNow();
    for (const auto& entry : license_->sub_products)
        if (moonbase::has_sub_product(*license_, entry.id, now))
            ids.addIfNotAlreadyThere(juce::String::fromUTF8(entry.id.c_str()));
    return ids;
}

void ActivationController::resetAddOns()
{
    ++addOnsGeneration_;
    addOns_ = {};
    addOnCall_ = 0;
    lapsesChecked_.clear();
    purchaseOpenedAt_.reset();
    leftForPurchase_ = false;
}

juce::String ActivationController::addOnName(const juce::String& subProductId) const
{
    const auto id = subProductId.toStdString();
    for (const auto& entry : addOns_.addOns)
        if (entry.id == id || entry.product.id == id)
            return juce::String::fromUTF8(entry.product.name.c_str());
    return {};
}

void ActivationController::activateSubProduct(const juce::String& subProductId,
                                              std::function<void(bool, const juce::String&)> onComplete)
{
    runSubProductAction(subProductId, /*trial*/ false, std::move(onComplete));
}

void ActivationController::requestSubProductTrial(const juce::String& subProductId,
                                                  std::function<void(bool, const juce::String&)> onComplete)
{
    runSubProductAction(subProductId, /*trial*/ true, std::move(onComplete));
}

void ActivationController::runSubProductAction(const juce::String& subProductId, bool trial,
                                               std::function<void(bool, const juce::String&)> onComplete)
{
    if (licensing_ == nullptr || ! license_ || license_->trial)
    {
        const juce::String message = license_ && license_->trial
            ? juce::String("Add-ons are activated from a full license, not a trial.")
            : "Activate " + config_.resolvedProductName() + " first.";
        addOns_.actionError = message;
        sendChangeMessage();
        if (onComplete) onComplete(false, message);
        return;
    }

    if (addOns_.busyId.isNotEmpty())
    {
        if (onComplete) onComplete(false, "Another add-on is still being set up. Try again in a moment.");
        return;
    }

    // Named in the copy when the add-ons have been listed.
    const auto name = addOnName(subProductId);
    const auto subject = name.isNotEmpty() ? name : juce::String("This add-on");
    const auto object = name.isNotEmpty() ? name : juce::String("this add-on");
    const auto account = config_.brandAccountName();

    const auto call = ++addOnCalls_;
    addOnCall_ = call;
    addOns_.busyId = subProductId;
    addOns_.actionError.clear();
    sendChangeMessage();

    const auto token = license_->token;
    const auto activationId = license_->activation_id;
    const auto id = subProductId.toStdString();
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, licensing, token, activationId, id, trial, call, subject, object, account,
                        onComplete, onlineCalls = onlineCalls_]() mutable
    {
        // Takes turns with a forced refresh until the answer is queued (see onlineCalls_).
        std::lock_guard<std::mutex> oneAtATime(*onlineCalls);

        std::optional<moonbase::license> result;
        bool licenseRefused = false;
        juce::String diag;
        juce::String userMessage;
        try
        {
            // licensing holds the store's lock from the request until it stores
            // the answer, so answers reach the store in the order the server gave
            // them, whichever instance asked. Only over a stored copy of this
            // activation: one cleared, deactivated or replaced meanwhile is left
            // as it is. (Nothing stored can also mean the license's first save
            // failed; the next refresh writes it, see refreshLicense.)
            const auto stillThisLicense = [licensing, activationId]
            {
                const auto stored = licensing->store().load_local_license();
                return stored.has_value() && stored->activation_id == activationId;
            };
            result = trial ? licensing->request_sub_product_trial(token, id, stillThisLicense)
                           : licensing->activate_sub_product(token, id, stillThisLicense);
        }
        catch (const moonbase::no_eligible_license_error& ex)
        {
            diag = juce::String::fromUTF8(ex.what());
            userMessage = "No seat of " + object + " is free for this computer. Free one in your " + account
                        + " account, or buy one.";
        }
        catch (const moonbase::operation_not_supported_error& ex)
        {
            diag = juce::String::fromUTF8(ex.what());
            userMessage = subject + (trial ? " can't be tried on this computer." : " can't be activated on this computer.");
        }
        catch (const moonbase::license_expired_error& ex)
        {
            licenseRefused = true;
            diag = juce::String::fromUTF8(ex.what());
            userMessage = "Your license has expired.";
        }
        catch (const moonbase::license_invalid_error& ex)
        {
            licenseRefused = true;
            diag = juce::String::fromUTF8(ex.what());
            userMessage = "Your license is no longer valid on this computer.";
        }
        catch (const std::exception& ex)
        {
            diag = describeError(ex);
            userMessage = addOnFailure(ex,
                                       trial ? "Couldn't start the trial of " + object : "Couldn't activate " + object,
                                       "Moonbase couldn't find " + object + ".");
        }


        juce::MessageManager::callAsync([safe, result, licenseRefused, diag, userMessage, trial, call, activationId,
                                         onComplete]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr)
                return;
            // Unless the license went in the meantime, which cleared it already.
            if (self->addOnCall_ == call)
            {
                self->addOnCall_ = 0;
                self->addOns_.busyId.clear();
            }

            // Only onto the license it was asked from: one deactivated, cleared or
            // replaced since keeps what it has now.
            const bool sameLicense = self->license_ && self->license_->activation_id == activationId;
            const juce::String changed = "Your license changed while this was in progress.";

            if (result)
            {
                const bool applies = sameLicense && result->activation_id == activationId;
                if (applies)
                    self->setLicense(std::move(result)); // stored already, by licensing
                self->sendChangeMessage();
                if (applies && self->addOns_.phase != AddOnsInfo::Phase::Idle)
                    self->listAddOns(/*pickUpSeats*/ false, {}); // show what changed
                if (onComplete) onComplete(applies, applies ? juce::String() : changed);
                return;
            }

            self->emitDiagnostic(juce::String(trial ? "requestSubProductTrial" : "activateSubProduct")
                                 + " failed: " + diag);
            if (! sameLicense)
            {
                self->sendChangeMessage();
                if (onComplete) onComplete(false, changed);
                return;
            }

            self->addOns_.actionError = userMessage;
            self->sendChangeMessage();
            // The license itself was refused: re-check it, which locks as a
            // refresh does when the server stands by that. Not over a
            // deactivation on its way, which settles it either way.
            if (licenseRefused && ! self->busy_)
                self->refreshLicense(true);
            if (onComplete) onComplete(false, userMessage);
        });
    });
}

void ActivationController::loadAddOns(std::function<void(bool)> onComplete)
{
    listAddOns(/*pickUpSeats*/ true, std::move(onComplete));
}

void ActivationController::listAddOns(bool pickUpSeats, std::function<void(bool)> onComplete)
{
    if (licensing_ == nullptr || ! license_)
    {
        if (onComplete) onComplete(false);
        return;
    }

    const auto generation = ++addOnsGeneration_;
    addOns_.phase = AddOnsInfo::Phase::Loading;
    addOns_.error.clear();
    sendChangeMessage();

    const auto token = license_->token;
    juce::WeakReference<ActivationController> safe(this);
    auto licensing = licensing_;

    threadPool_.addJob([safe, generation, licensing, token, pickUpSeats, onComplete]() mutable
    {
        std::optional<moonbase::sub_product_catalog> catalog;
        bool licenseRefused = false;
        juce::String diag;
        juce::String userMessage;
        try
        {
            catalog = licensing->list_sub_products(token);
        }
        catch (const moonbase::license_expired_error& ex)
        {
            licenseRefused = true;
            diag = juce::String::fromUTF8(ex.what());
            userMessage = "Your license has expired.";
        }
        catch (const moonbase::license_invalid_error& ex)
        {
            licenseRefused = true;
            diag = juce::String::fromUTF8(ex.what());
            userMessage = "Your license is no longer valid on this computer.";
        }
        catch (const std::exception& ex)
        {
            diag = describeError(ex);
            userMessage = addOnFailure(ex, "Couldn't load the add-ons",
                                       "Moonbase couldn't find the add-ons for this license.");
        }

        juce::MessageManager::callAsync([safe, generation, catalog, licenseRefused, diag, userMessage,
                                         pickUpSeats, onComplete]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr)
                return;
            if (generation != self->addOnsGeneration_.load())
            {
                if (onComplete) onComplete(false); // superseded, or the license went
                return;
            }

            self->addOns_.phase = AddOnsInfo::Phase::Ready;
            if (catalog)
            {
                self->addOns_.currency = juce::String::fromUTF8(catalog->currency.c_str());
                self->addOns_.addOns = std::move(catalog->sub_products);
                self->addOns_.error.clear();
            }
            else
            {
                self->emitDiagnostic("loadAddOns failed: " + diag);
                self->addOns_.error = userMessage;
            }
            self->sendChangeMessage();
            // As for an add-on call (the generation above means it is still the
            // license the list was asked for).
            if (licenseRefused && ! self->busy_)
                self->refreshLicense(true);

            // Bought since the license was last checked: checking it takes the
            // seat, as it would at the next launch. Once, so an add-on the
            // server still won't seat can't start a loop.
            const bool takeSeat = catalog && pickUpSeats && self->listsSeatToTake();
            if (onComplete) onComplete(catalog.has_value());

            // The callback may have destroyed the controller (a host closing its
            // editor, say) or cleared the license, so look again before going on.
            self = safe.get();
            if (takeSeat && self != nullptr && self->listsSeatToTake())
            {
                self->emitDiagnostic("An add-on the customer owns has a seat free; re-checking the license to take it.");
                self->refreshAndListAddOns();
            }
        });
    });
}

bool ActivationController::listsSeatToTake() const
{
    if (! config_.autoActivateSubProducts || ! license_ || license_->trial
        || license_->method != moonbase::activation_method::online)
        return false;

    const auto now = clockNow();
    for (const auto& entry : addOns_.addOns)
    {
        const bool owned = entry.product.owned || entry.product.subscribed;
        if (! entry.included_in_license || ! owned || entry.revoked)
            continue;

        // On a seat here already: nothing to take. A trial still unlocking one
        // the customer has since bought gives way to a seat at the next check.
        const auto* held = moonbase::find_sub_product(*license_, entry.id);
        if (held != nullptr && ! held->trial && moonbase::has_sub_product(*license_, entry.id, now))
            continue;

        // A seat is free, or the list says this device has one that the license
        // doesn't show (an answer that was never stored, say).
        if (entry.active || (entry.seats && entry.seats->remaining > 0))
            return true;
    }
    return false;
}

void ActivationController::refreshAddOns()
{
    if (! license_)
        return;

    addOns_.actionError.clear();
    refreshAndListAddOns();
}

void ActivationController::refreshAndListAddOns()
{
    // Not over a deactivation on its way (see refreshLicense).
    if (! license_ || busy_)
        return;

    // An offline license is never re-validated; its add-ons are just listed.
    if (license_->method == moonbase::activation_method::offline)
    {
        listAddOns(/*pickUpSeats*/ false, {});
        return;
    }

    addOns_.phase = AddOnsInfo::Phase::Loading;
    addOns_.error.clear();
    sendChangeMessage();

    // The license was just checked, so the list that follows has no seat left
    // to take that checking again would.
    juce::WeakReference<ActivationController> safe(this);
    refreshLicense(true, [safe](bool)
    {
        if (auto* self = safe.get(); self != nullptr && self->license_)
            self->listAddOns(/*pickUpSeats*/ false, {});
    });
}

void ActivationController::setForegroundCheck(std::function<bool()> isInForeground)
{
    isInForeground_ = std::move(isInForeground);
}

void ActivationController::checkPurchaseReturn()
{
    if (! purchaseOpenedAt_)
        return;

    if (std::chrono::steady_clock::now() - *purchaseOpenedAt_ > std::chrono::hours(1))
    {
        purchaseOpenedAt_.reset(); // whatever happened, it wasn't a purchase coming back
        return;
    }

    const bool inForeground = isInForeground_ ? isInForeground_() : juce::Process::isForegroundProcess();
    if (! inForeground)
    {
        leftForPurchase_ = true;
        return;
    }
    // Back, but a deactivation is on its way: wait for it to settle (losing the
    // license forgets the purchase too).
    if (! leftForPurchase_ || ! license_ || busy_)
        return;

    purchaseOpenedAt_.reset();
    leftForPurchase_ = false;
    emitDiagnostic("Back from the browser after opening an add-on's buy link; refreshing the add-ons.");
    refreshAddOns();
}

bool ActivationController::buySubProduct(const juce::String& subProductId)
{
    const auto id = subProductId.toStdString();
    for (const auto& entry : addOns_.addOns)
    {
        if ((entry.id != id && entry.product.id != id) || entry.buy_url.empty())
            continue;

        const juce::URL link(juce::String::fromUTF8(entry.buy_url.c_str()));
        const bool opened = openBrowser(link);
        if (! opened)
        {
            emitDiagnostic("Couldn't open the browser to buy an add-on: " + link.toString(false));
            addOns_.actionError = "Couldn't open your browser to buy "
                                + juce::String::fromUTF8(entry.product.name.c_str()) + ".";
            sendChangeMessage();
            return false;
        }

        // Refresh once the customer is back from the browser (checkPurchaseReturn).
        purchaseOpenedAt_ = std::chrono::steady_clock::now();
        leftForPurchase_ = false;
        return true;
    }
    return false;
}

//==============================================================================
int ActivationController::trialDaysRemaining() const
{
    if (! license_ || ! license_->expires_at)
        return 0;
    return daysUntil(*license_->expires_at);
}

int ActivationController::daysUntil(std::chrono::system_clock::time_point when) const
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when - clockNow()).count();
    if (seconds <= 0)
        return 0;
    return static_cast<int>((seconds + 86399) / 86400); // ceil to whole days
}

bool ActivationController::trialIsEndless() const
{
    return license_ && license_->trial && ! license_->expires_at;
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
    const bool sameActivation = license_ && value && license_->activation_id == value->activation_id;
    license_ = std::move(value);
    if (license_)
    {
        lockReason_ = LockReason::None;
        notCovered_.reset();
    }
    // The add-ons were listed for the activation that just went or was replaced
    // (an activation here, say), along with anything still on its way for it.
    if (! sameActivation)
        resetAddOns();
    // Publish for the audio thread (this always runs on the message thread).
    licensed_.store(license_.has_value(), std::memory_order_release);
    publishSubProductFlags();
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
    auto subProducts = activeSubProducts();
    if (licenseReported_ && token == reportedToken_ && subProducts == reportedSubProducts_)
        return;

    licenseReported_ = true;
    reportedToken_ = std::move(token);
    reportedSubProducts_ = std::move(subProducts);
    if (onLicenseChanged)
        onLicenseChanged(license_.has_value());
}

ActivationController::Screen ActivationController::screenForCurrentLicense() const
{
    if (! license_)
        return notCovered_ ? Screen::NotCovered : Screen::Welcome;
    // A backend-granted trial license shows the trial view (days left / unlock);
    // a full license shows the details view.
    return license_->trial ? Screen::Trial : Screen::Details;
}

void ActivationController::applyLicense(std::optional<moonbase::license> value)
{
    setLicense(std::move(value));
    expiredTrial_.reset();
    notCovered_.reset();
    statusMessage_.clear();

    // A validated license that outranks the running app routes to the update
    // screen first (unless that version was skipped); "Skip this update" then
    // falls through to the normal Details / Trial screen.
    const auto dest = screenForCurrentLicense();

    // A refresh of the license whose add-ons are open (after a purchase, say)
    // keeps them open.
    if (screen_ == Screen::AddOns && dest == Screen::Details)
    {
        sendChangeMessage();
        return;
    }

    if ((dest == Screen::Details || dest == Screen::Trial) && config_.autoPresentUpdate
        && updateAvailable() && ! updateDismissedForCurrentLicense())
    {
        updateFromLicenseView_ = false; // auto-routed, not from the badge
        beginUpdateFlow(dest);
        return;
    }

    setScreen(dest);
}

void ActivationController::lock(LockReason reason)
{
    lockReason_ = reason;
    applyLicense(std::nullopt);
}

void ActivationController::showTrialExpired(moonbase::license expired)
{
    // The plugin must stay locked, so license_ stays empty; the ended trial is
    // held separately for the Expired screen to show the product + end date.
    expiredTrial_ = std::move(expired);
    notCovered_.reset();
    lockReason_ = LockReason::Expired;
    setLicense(std::nullopt);
    statusMessage_.clear();
    setScreen(Screen::Expired);
}

void ActivationController::showNotCovered(NotCoveredInfo info)
{
    // Locked like an ended trial, but the license stays stored: it still covers
    // the versions it did, and an upgrade makes it cover this one.
    expiredTrial_.reset();
    notCovered_ = std::move(info);
    lockReason_ = LockReason::NotCovered;
    setLicense(std::nullopt);
    statusMessage_.clear();
    setScreen(Screen::NotCovered);
}

ActivationController::NotCoveredInfo ActivationController::notCoveredFrom(
    const moonbase::release_not_covered_error& error, const moonbase::license& fallback)
{
    NotCoveredInfo info;
    info.license = error.license() ? *error.license() : fallback;
    info.appVersion = juce::String::fromUTF8(error.app_version().c_str());
    info.releaseAccess = error.release_access() ? error.release_access() : info.license.release_access;
    if (error.upgrade_url())
        info.upgradeUrl = juce::String::fromUTF8(error.upgrade_url()->c_str());
    return info;
}

void ActivationController::storeLicense(const moonbase::license& value, const char* context)
{
    try
    {
        auto guard = licensing_->store().lock_for_update();
        licensing_->store().store_local_license(value);
    }
    catch (const moonbase::storage_error& ex)
    {
        emitDiagnostic(juce::String(context) + ", but couldn't persist the license: " + ex.what());
    }
}

bool ActivationController::openBrowser(const juce::URL& url)
{
    // On Windows this is ShellExecute, which fails when no default browser is
    // set; config.openBrowser routes the link elsewhere.
    return config_.openBrowser ? config_.openBrowser(url) : url.launchInDefaultBrowser();
}

void ActivationController::upgradeLicense()
{
    if (! ensureReady())
        return;

    if (screen_ == Screen::UpdateAvailable)
    {
        if (updateInfo_.upgradeUrl.isNotEmpty() && ! openBrowser(juce::URL(updateInfo_.upgradeUrl)))
            emitDiagnostic("Couldn't open the browser for the upgrade: " + updateInfo_.upgradeUrl);
        return;
    }

    if (! notCovered_ || license_)
        return;

    if (notCovered_->upgradeUrl.isEmpty())
    {
        // Only Moonbase knows where to buy the upgrade. Its activation page
        // offers it to a signed-in customer, then activates this computer.
        beginOnlineActivation();
        return;
    }

    if (! openBrowser(juce::URL(notCovered_->upgradeUrl)))
    {
        emitDiagnostic("Couldn't open the browser for the upgrade: " + notCovered_->upgradeUrl);
        setScreen(Screen::NotCovered, "Couldn't open your browser. Upgrade at " + notCovered_->upgradeUrl);
    }
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
                // Moonbase judges this by all of the customer's licenses, as
                // validating the new version does: the seat moves to one that
                // covers it.
                self->updateInfo_.covered = info->release_covered;
                self->updateInfo_.releaseAccess = info->release_access;
                self->updateInfo_.upgradeUrl = info->upgrade_url ? juce::String::fromUTF8(info->upgrade_url->c_str())
                                                                 : juce::String();
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
        std::optional<NotCoveredInfo> notCovered; // the licenses don't cover the release after all
        juce::String diag;
        juce::String userMessage;
        try
        {
            target = inventory.get_download_url(platformName, token);
        }
        catch (const moonbase::release_not_covered_error& ex)
        {
            diag = juce::String("startUpdateDownload refused: ") + ex.what();
            notCovered = notCoveredFrom(ex, {});
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

        juce::MessageManager::callAsync([safe, generation, target, notCovered, diag, userMessage]() mutable
        {
            auto* self = safe.get();
            if (self == nullptr || generation != self->updateGeneration_.load())
                return;
            if (target)
            {
                self->beginFileDownload(*target);
            }
            else if (notCovered)
            {
                // The screen swaps the download for the upgrade.
                self->emitDiagnostic(diag);
                auto& info = self->updateInfo_;
                info.covered = false;
                info.canDownload = false;
                info.releaseAccess = notCovered->releaseAccess;
                info.upgradeUrl = notCovered->upgradeUrl;
                info.phase = UpdateInfo::Phase::Ready;
                info.progress = 0.0;
                self->sendChangeMessage();
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
                                            juce::String error, bool canDownload, bool covered,
                                            juce::String upgradeUrl)
{
    ++generation_;
    ++updateGeneration_;
    stopTimer();
    stopLicenseWatch(); // a preview stays exactly as set
    pollInFlight_ = false;
    busy_ = false;

    setLicense(std::move(license));
    expiredTrial_.reset();
    notCovered_.reset();
    statusMessage_.clear();

    updateInfo_ = {};
    updateInfo_.phase = phase;
    updateInfo_.currentVersion = config_.resolvedApplicationVersion();
    if (license_ && license_->licensed_product.current_release_version)
        updateInfo_.newVersion = juce::String(*license_->licensed_product.current_release_version);
    updateInfo_.releaseNotes = std::move(releaseNotes);
    updateInfo_.progress = progressValue;
    updateInfo_.error = std::move(error);
    updateInfo_.canDownload = canDownload && covered;
    updateInfo_.covered = covered;
    if (! covered && license_)
        updateInfo_.releaseAccess = license_->release_access;
    updateInfo_.upgradeUrl = std::move(upgradeUrl);

    screen_ = Screen::UpdateAvailable;
    sendSynchronousChangeMessage();
}

void ActivationController::setPreviewNotCovered(moonbase::license license, juce::String upgradeUrl)
{
    ++generation_;
    stopTimer();
    stopLicenseWatch(); // a preview stays exactly as set
    pollInFlight_ = false;
    busy_ = false;
    pendingRequest_.reset();
    offlineError_.clear();

    NotCoveredInfo info;
    if (const auto version = moonbase::parse_app_version(config_.resolvedApplicationVersion().toStdString()))
        info.appVersion << juce::String(version->major) << "." << juce::String(version->minor) << "."
                        << juce::String(version->patch);
    info.releaseAccess = license.release_access;
    info.license = std::move(license);
    info.upgradeUrl = std::move(upgradeUrl);

    expiredTrial_.reset();
    notCovered_ = std::move(info);
    lockReason_ = LockReason::NotCovered;
    setLicense(std::nullopt);
    statusMessage_.clear();
    screen_ = Screen::NotCovered;
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
