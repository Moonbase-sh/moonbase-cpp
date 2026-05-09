// Drop-in bridge between the Moonbase C++ activation SDK and JUCE.
//
// This file is reference code. It is not part of the SDK build target — copy
// it into your JUCE project and link `moonbase::licensing` alongside the JUCE
// modules you already use.
//
// Required JUCE modules:
//   - juce_core           (always)
//   - juce_cryptography   (for juce::OnlineUnlockStatus, transitively pulled in by juce_product_unlocking)
//   - juce_product_unlocking (for juce::OnlineUnlockStatus)
//   - juce_audio_processors  (optional — enables host metadata via juce::PluginHostType)
//
// If juce_audio_processors is in your build but the auto-detect fails on your
// toolchain, define MOONBASE_JUCE_HAS_AUDIO_PROCESSORS=1 before including this
// header to force-enable host metadata.
//
// See docs/juce.md in the moonbase-cpp repository for the full integration guide.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <moonbase/moonbase.hpp>

#include <juce_core/juce_core.h>
#include <juce_product_unlocking/juce_product_unlocking.h>

#if !defined(MOONBASE_JUCE_HAS_AUDIO_PROCESSORS)
  #if defined(__has_include)
    #if __has_include(<juce_audio_processors/juce_audio_processors.h>)
      #define MOONBASE_JUCE_HAS_AUDIO_PROCESSORS 1
    #else
      #define MOONBASE_JUCE_HAS_AUDIO_PROCESSORS 0
    #endif
  #else
    #define MOONBASE_JUCE_HAS_AUDIO_PROCESSORS 0
  #endif
#endif

#if MOONBASE_JUCE_HAS_AUDIO_PROCESSORS
  #include <juce_audio_processors/juce_audio_processors.h>
#endif

namespace moonbase::juce_bridge {

// ---------------------------------------------------------------------------
// Fingerprinting
// ---------------------------------------------------------------------------

// Sources the device fingerprint from juce::SystemStats::getUniqueDeviceID(),
// which JUCE itself hashes from stable hardware identifiers. Requires JUCE 7+.
class MoonbaseJuceFingerprintProvider : public moonbase::fingerprint_provider
{
public:
    [[nodiscard]] std::string device_name() const override
    {
        return juce::SystemStats::getComputerName().toStdString();
    }

    [[nodiscard]] std::string device_id() const override
    {
        return juce::SystemStats::getUniqueDeviceID().toStdString();
    }
};

// ---------------------------------------------------------------------------
// Metadata helper
// ---------------------------------------------------------------------------

struct MoonbaseJuceMetadataOptions
{
    bool includeSystemInfo = true;   // OS, CPU, JUCE version, memory
    bool includeHostInfo = true;     // DAW host + plugin format (no-op without juce_audio_processors)
    bool includeLocaleInfo = false;  // language/region — opt in
    bool includeAppVersion = true;   // also fills licensing_options.application_version when unset
};

namespace detail {

inline void putIfAbsent(std::map<std::string, std::string>& map,
                        std::string key,
                        std::string value)
{
    if (value.empty())
        return;
    map.emplace(std::move(key), std::move(value));
}

#if MOONBASE_JUCE_HAS_AUDIO_PROCESSORS
inline std::string pluginFormatTag(juce::AudioProcessor::WrapperType wrapper)
{
    switch (wrapper)
    {
        case juce::AudioProcessor::wrapperType_VST:           return "VST";
        case juce::AudioProcessor::wrapperType_VST3:          return "VST3";
        case juce::AudioProcessor::wrapperType_AudioUnit:     return "AU";
        case juce::AudioProcessor::wrapperType_AudioUnitv3:   return "AUv3";
        case juce::AudioProcessor::wrapperType_AAX:           return "AAX";
        case juce::AudioProcessor::wrapperType_Standalone:    return "Standalone";
        case juce::AudioProcessor::wrapperType_LV2:           return "LV2";
        case juce::AudioProcessor::wrapperType_Unity:         return "Unity";
        case juce::AudioProcessor::wrapperType_Undefined:
        default:                                              return "Unknown";
    }
}
#endif

} // namespace detail

inline void applyJuceMetadata(moonbase::licensing_options& options,
                              const MoonbaseJuceMetadataOptions& opts = {})
{
    auto& meta = options.metadata;

    if (opts.includeSystemInfo)
    {
        detail::putIfAbsent(meta, "juce.version", juce::SystemStats::getJUCEVersion().toStdString());
        detail::putIfAbsent(meta, "juce.os", juce::SystemStats::getOperatingSystemName().toStdString());
        meta.emplace("juce.os.is64Bit", juce::SystemStats::isOperatingSystem64Bit() ? "true" : "false");
        detail::putIfAbsent(meta, "juce.cpu.model", juce::SystemStats::getCpuModel().trim().toStdString());
        detail::putIfAbsent(meta, "juce.cpu.vendor", juce::SystemStats::getCpuVendor().trim().toStdString());
        meta.emplace("juce.cpu.cores", std::to_string(juce::SystemStats::getNumPhysicalCpus()));
        meta.emplace("juce.cpu.threads", std::to_string(juce::SystemStats::getNumCpus()));
        meta.emplace("juce.memoryMb", std::to_string(juce::SystemStats::getMemorySizeInMegabytes()));
    }

#if MOONBASE_JUCE_HAS_AUDIO_PROCESSORS
    if (opts.includeHostInfo)
    {
        const juce::PluginHostType host;
        detail::putIfAbsent(meta, "juce.host.description", juce::String(host.getHostDescription()).toStdString());
        meta.emplace("juce.host.format", detail::pluginFormatTag(juce::PluginHostType::getPluginLoadedAs()));
    }
#else
    (void) opts.includeHostInfo;
#endif

    if (opts.includeLocaleInfo)
    {
        detail::putIfAbsent(meta, "juce.locale.display", juce::SystemStats::getDisplayLanguage().toStdString());
        detail::putIfAbsent(meta, "juce.locale.user", juce::SystemStats::getUserLanguage().toStdString());
        detail::putIfAbsent(meta, "juce.locale.region", juce::SystemStats::getUserRegion().toStdString());
    }

    if (auto* app = juce::JUCEApplicationBase::getInstance())
    {
        detail::putIfAbsent(meta, "juce.app.name", app->getApplicationName().toStdString());

        if (opts.includeAppVersion && !options.application_version)
        {
            const auto version = app->getApplicationVersion().toStdString();
            if (!version.empty())
                options.application_version = version;
        }
    }
}

// ---------------------------------------------------------------------------
// Unlocker
// ---------------------------------------------------------------------------

// Bridge between juce::OnlineUnlockStatus and moonbase::licensing.
//
// Moonbase uses a JWT/browser activation flow rather than JUCE's email-and-
// password keyfile challenge, but the inherited isUnlocked() and
// getExpiryTime() are kept consistent with the underlying moonbase license:
// after every state change we synthesize a self-signed JUCE keyfile from the
// validated moonbase license and feed it to applyKeyFile(), which is the only
// public path into juce::OnlineUnlockStatus's private status ValueTree. The
// process-local RSA keypair never leaves the process, so the JUCE keyfile is
// just an internal data shape, not a security boundary.
class MoonbaseUnlockStatus : public juce::OnlineUnlockStatus
{
public:
    explicit MoonbaseUnlockStatus(moonbase::licensing_options options,
                                  std::shared_ptr<moonbase::license_store> store = nullptr,
                                  std::shared_ptr<moonbase::fingerprint_provider> fingerprint
                                      = std::make_shared<MoonbaseJuceFingerprintProvider>(),
                                  juce::String websiteName = "moonbase.sh")
        : productId_(options.product_id),
          websiteName_(std::move(websiteName)),
          licensing_(std::make_shared<moonbase::licensing>(
              std::move(options), std::move(store), std::move(fingerprint)))
    {
        juce::RSAKey::createKeyPair(juceUnlockPublicKey_, juceUnlockPrivateKey_, 512);
    }

    // ---- Moonbase-facing API -------------------------------------------------

    // Loads any previously stored license and validates its JWT. Call this once
    // from your AudioProcessor constructor (or app startup). Safe to call again
    // after reconfiguring the license store.
    //
    // With online=true (default) this calls licensing.validate_token_online,
    // which adds an API round-trip subject to the cadence + grace period
    // configured on licensing_options. The call is synchronous and blocks on
    // the calling thread when it actually hits the network — within the
    // throttle window (online_validation_min_interval, default 5 minutes) it's
    // a single timestamp comparison and returns immediately. If you need to
    // avoid any network I/O, pass online=false to use local-only validation.
    //
    // Offline-activated tokens are validated locally regardless of this flag.
    bool tryLoadStoredLicense(bool online = true)
    {
        ++validationGeneration_;
        try
        {
            auto stored = licensing_->store().load_local_license();
            if (!stored)
            {
                setUnlocked(std::nullopt);
                return false;
            }

            auto validated = online
                ? licensing_->validate_token_online(stored->token)
                : licensing_->validate_token_local(stored->token);

            // Persist refreshed token so the cadence/grace clock advances
            // across restarts. Storage failures are non-fatal here.
            if (validated.token != stored->token)
            {
                try { licensing_->store().store_local_license(validated); }
                catch (const moonbase::storage_error&) {}
            }

            setUnlocked(std::move(validated));
            return true;
        }
        catch (const moonbase::license_invalid_error&)
        {
            setUnlocked(std::nullopt);
            return false;
        }
        catch (const moonbase::license_expired_error&)
        {
            setUnlocked(std::nullopt);
            return false;
        }
        catch (const std::exception&)
        {
            // Transport failure past the grace period (or any other unexpected
            // error). Treat as locked rather than letting it propagate into
            // the host's plugin-load path.
            setUnlocked(std::nullopt);
            return false;
        }
    }

    enum class AsyncValidationOutcome
    {
        NoStoredLicense,    // No license persisted on disk.
        LocalInvalid,       // Stored license failed local validation (signature/device/exp).
        OfflineToken,       // Token is offline-activated; no online check performed.
        Refreshed,          // Online check returned a valid license (either freshly refreshed
                            // by the API or, within the grace period, the cached local copy).
        LockedInvalid,      // Server explicitly rejected the token as invalid.
        LockedExpired,      // Server explicitly rejected the token as expired.
        Unreachable,        // Past grace period; transport failed. Bridge is now locked.
    };

    struct AsyncValidationResult
    {
        AsyncValidationOutcome outcome;
        std::optional<moonbase::license> license; // The unlocked license, or empty if locked.
    };

    // Optimistic, non-blocking variant of tryLoadStoredLicense.
    //
    // Threading: state mutation and the callback always run on the JUCE
    // message thread, regardless of which thread invokes this method. The
    // local load + validate is CPU-only (no JUCE state mutation) and runs on
    // the calling thread synchronously; the resulting state is then applied
    // via juce::MessageManager::callAsync. For online-activated tokens, the
    // API round-trip runs on a juce::Thread and its result is also delivered
    // through callAsync.
    //
    // Staleness: every call captures a generation number that is checked
    // before any state mutation or callback fires. clearLicense() and a
    // successful pollPendingActivation() bump the generation, as does each
    // new tryLoadStoredLicenseAsync call. Continuations whose generation
    // doesn't match the current one are dropped silently — both state and
    // callback. This prevents a slow online check from resurrecting a license
    // the user just cleared, or clobbering a freshly activated one.
    //
    // Lifetime: licensing_ is a shared_ptr so the background thread keeps the
    // SDK alive even if the bridge is destroyed mid-request. A
    // juce::WeakReference protects the message-thread continuation from
    // touching a destroyed bridge.
    //
    // Within the grace period a transport failure is reported as Refreshed
    // with the cached license (effectively "still valid"); beyond grace it
    // becomes Unreachable and the bridge is locked.
    void tryLoadStoredLicenseAsync(std::function<void(AsyncValidationResult)> onComplete = {})
    {
        const auto generation = ++validationGeneration_;
        juce::WeakReference<MoonbaseUnlockStatus> safeThis(this);

        // Marshals state mutation + callback to the message thread, gated on
        // the captured generation matching the current one. Stale calls do
        // nothing and silently drop the callback.
        auto deliver =
            [safeThis, generation](AsyncValidationResult result,
                                   std::function<void(AsyncValidationResult)> cb,
                                   bool persistRefreshed) mutable
            {
                juce::MessageManager::callAsync(
                    [safeThis, generation, result = std::move(result),
                     cb = std::move(cb), persistRefreshed]() mutable
                    {
                        auto* self = safeThis.get();
                        if (self == nullptr
                            || generation != self->validationGeneration_.load())
                            return;

                        switch (result.outcome)
                        {
                        case AsyncValidationOutcome::Refreshed:
                            if (result.license)
                            {
                                if (persistRefreshed)
                                {
                                    try { self->licensing_->store().store_local_license(*result.license); }
                                    catch (const moonbase::storage_error&) {}
                                }
                                self->setUnlocked(*result.license);
                            }
                            break;
                        case AsyncValidationOutcome::OfflineToken:
                            if (result.license)
                                self->setUnlocked(*result.license);
                            break;
                        case AsyncValidationOutcome::NoStoredLicense:
                        case AsyncValidationOutcome::LocalInvalid:
                        case AsyncValidationOutcome::LockedInvalid:
                        case AsyncValidationOutcome::LockedExpired:
                        case AsyncValidationOutcome::Unreachable:
                            self->setUnlocked(std::nullopt);
                            break;
                        }

                        if (cb)
                            cb(std::move(result));
                    });
            };

        // Step 1: synchronous local load + validate (CPU only, no JUCE state).
        std::optional<moonbase::license> local;
        try
        {
            auto stored = licensing_->store().load_local_license();
            if (!stored)
            {
                deliver({AsyncValidationOutcome::NoStoredLicense, std::nullopt},
                        std::move(onComplete), false);
                return;
            }
            local = licensing_->validate_token_local(stored->token);
        }
        catch (const std::exception&)
        {
            deliver({AsyncValidationOutcome::LocalInvalid, std::nullopt},
                    std::move(onComplete), false);
            return;
        }

        if (local->method == moonbase::activation_method::offline)
        {
            deliver({AsyncValidationOutcome::OfflineToken, local},
                    std::move(onComplete), false);
            return;
        }

        // Optimistic unlock from the local check, marshalled to the message
        // thread (no callback yet). The final state may override this once
        // the online check resolves.
        deliver({AsyncValidationOutcome::Refreshed, *local}, {}, false);

        // Step 2: online check on a background thread.
        auto licensingHandle = licensing_;
        const auto token = local->token;
        auto cb = std::move(onComplete);

        juce::Thread::launch(
            [licensingHandle, token, deliver = std::move(deliver),
             cb = std::move(cb)]() mutable
            {
                AsyncValidationResult result{AsyncValidationOutcome::Refreshed, std::nullopt};
                bool persistRefreshed = true;
                try
                {
                    auto refreshed = licensingHandle->validate_token_online(token);
                    result = {AsyncValidationOutcome::Refreshed, std::move(refreshed)};
                }
                catch (const moonbase::license_invalid_error&)
                {
                    result = {AsyncValidationOutcome::LockedInvalid, std::nullopt};
                    persistRefreshed = false;
                }
                catch (const moonbase::license_expired_error&)
                {
                    result = {AsyncValidationOutcome::LockedExpired, std::nullopt};
                    persistRefreshed = false;
                }
                catch (const std::exception&)
                {
                    // validate_token_online only throws non-license exceptions
                    // when grace has elapsed.
                    result = {AsyncValidationOutcome::Unreachable, std::nullopt};
                    persistRefreshed = false;
                }

                deliver(std::move(result), std::move(cb), persistRefreshed);
            });
    }

    enum class RevokeOutcome
    {
        Revoked,        // Server-side revoke succeeded (or token was already gone). Bridge is now locked.
        NoLicense,      // No license is currently loaded — nothing to revoke. Bridge state unchanged.
        NotRevokable,   // Token is offline-activated or a trial. Bridge state unchanged.
                        // Caller can fall back to clearLicense() for a local-only forget.
        Unreachable,    // Transport failure (network down, 5xx). Bridge state unchanged so the
                        // caller can retry; no seat has been freed server-side.
    };

    // Server-side revoke of the currently loaded activation. On success, frees
    // the seat on the Moonbase backend and clears the local store + bridge
    // state. On NotRevokable / Unreachable the bridge state is left alone so
    // the caller can decide between a local-only fallback (clearLicense()) or
    // a retry.
    //
    // Synchronous: blocks the calling thread on libcurl. Prefer
    // revokeActivationAsync() from UI threads.
    RevokeOutcome revokeActivation()
    {
        std::string token;
        std::string activationId;
        {
            const juce::ScopedLock lock(stateLock_);
            if (!current_)
                return RevokeOutcome::NoLicense;
            token = current_->token;
            activationId = current_->activation_id;
        }

        ++validationGeneration_;

        auto clearMatchingLocal = [&] {
            try
            {
                auto stored = licensing_->store().load_local_license();
                if (stored && stored->activation_id == activationId)
                    licensing_->store().delete_local_license();
            }
            catch (const moonbase::storage_error&) {}
        };

        try
        {
            licensing_->revoke_activation(token);
        }
        catch (const moonbase::operation_not_supported_error&)
        {
            return RevokeOutcome::NotRevokable;
        }
        catch (const moonbase::license_invalid_error&)
        {
            // Server says the token is unknown/invalid — treat as already gone.
            // The SDK didn't reach its own store-cleanup, so do it here, gated
            // on activation_id so we never wipe an unrelated cached license.
            clearMatchingLocal();
            setUnlocked(std::nullopt);
            return RevokeOutcome::Revoked;
        }
        catch (const moonbase::license_expired_error&)
        {
            clearMatchingLocal();
            setUnlocked(std::nullopt);
            return RevokeOutcome::Revoked;
        }
        catch (const std::exception&)
        {
            return RevokeOutcome::Unreachable;
        }

        // SDK facade has already cleared the matching license from the store.
        setUnlocked(std::nullopt);
        return RevokeOutcome::Revoked;
    }

    // Non-blocking variant of revokeActivation.
    //
    // Threading: state mutation, store cleanup, and the callback all run on
    // the JUCE message thread, regardless of which thread invokes this
    // method. The network call runs on a juce::Thread.
    //
    // Staleness: same generation-gating as tryLoadStoredLicenseAsync — if the
    // user calls clearLicense(), kicks off another async operation, or
    // finishes a new activation while a revoke is in flight, the older
    // continuation is dropped silently. Both state mutation AND the user
    // callback are dropped, so a stale revoke can never overwrite the UI of
    // a freshly activated license, and a destroyed bridge never receives
    // its callback.
    //
    // Lifetime: licensing_ is a shared_ptr so the background thread keeps the
    // SDK alive even if the bridge is destroyed mid-request. A
    // juce::WeakReference protects the message-thread continuation from
    // touching a destroyed bridge.
    void revokeActivationAsync(std::function<void(RevokeOutcome)> onComplete = {})
    {
        std::string token;
        std::string activationId;
        {
            const juce::ScopedLock lock(stateLock_);
            if (current_)
            {
                token = current_->token;
                activationId = current_->activation_id;
            }
        }

        const auto generation = ++validationGeneration_;
        juce::WeakReference<MoonbaseUnlockStatus> safeThis(this);
        auto licensingHandle = licensing_;
        auto cb = std::move(onComplete);

        if (token.empty())
        {
            // Fast path: nothing loaded. Still go through the message-thread
            // gate so a destroyed bridge or a superseding op silently drops
            // the callback, matching the rest of the async surface.
            juce::MessageManager::callAsync(
                [safeThis, generation, cb = std::move(cb)]() mutable
                {
                    auto* self = safeThis.get();
                    if (self == nullptr
                        || generation != self->validationGeneration_.load())
                        return;
                    if (cb)
                        cb(RevokeOutcome::NoLicense);
                });
            return;
        }

        juce::Thread::launch(
            [licensingHandle, token, activationId, generation, safeThis,
             cb = std::move(cb)]() mutable
            {
                RevokeOutcome outcome = RevokeOutcome::Revoked;
                try
                {
                    licensingHandle->revoke_activation(token);
                }
                catch (const moonbase::operation_not_supported_error&)
                {
                    outcome = RevokeOutcome::NotRevokable;
                }
                catch (const moonbase::license_invalid_error&)
                {
                    outcome = RevokeOutcome::Revoked;
                }
                catch (const moonbase::license_expired_error&)
                {
                    outcome = RevokeOutcome::Revoked;
                }
                catch (const std::exception&)
                {
                    outcome = RevokeOutcome::Unreachable;
                }

                juce::MessageManager::callAsync(
                    [safeThis, generation, outcome, activationId,
                     licensingHandle, cb = std::move(cb)]() mutable
                    {
                        auto* self = safeThis.get();
                        if (self == nullptr
                            || generation != self->validationGeneration_.load())
                            return;

                        if (outcome == RevokeOutcome::Revoked)
                        {
                            // Cover the invalid/expired server-response path
                            // where the SDK threw before its own store
                            // cleanup. activation_id matching ensures a stale
                            // revoke whose generation somehow still matches
                            // can't wipe an unrelated cached license.
                            try
                            {
                                auto stored = licensingHandle->store().load_local_license();
                                if (stored && stored->activation_id == activationId)
                                    licensingHandle->store().delete_local_license();
                            }
                            catch (const moonbase::storage_error&) {}

                            self->setUnlocked(std::nullopt);
                        }

                        if (cb)
                            cb(outcome);
                    });
            });
    }

    // Begins a new browser activation. Returns the URL to hand to
    // juce::URL::launchInDefaultBrowser. Throws moonbase::api_error on
    // network/server failure.
    juce::URL beginActivation()
    {
        const juce::ScopedLock lock(stateLock_);
        pendingRequest_ = licensing_->request_activation();
        return juce::URL(juce::String(pendingRequest_->browser_url));
    }

    // Non-blocking poll. Returns true the first call after the user finishes
    // activation in the browser; returns false otherwise. Run from a juce::Timer
    // on the message thread.
    bool pollPendingActivation()
    {
        std::optional<moonbase::activation_request> request;
        {
            const juce::ScopedLock lock(stateLock_);
            request = pendingRequest_;
        }
        if (!request)
            return false;

        auto fulfilled = licensing_->get_requested_activation(*request);
        if (!fulfilled)
            return false;

        ++validationGeneration_; // invalidate any in-flight async revalidation

        try
        {
            licensing_->store().store_local_license(*fulfilled);
        }
        catch (const moonbase::storage_error&)
        {
            // Activation succeeded but persistence failed; still mark unlocked
            // for this session so the user isn't locked out by a bad disk.
        }

        const juce::ScopedLock lock(stateLock_);
        pendingRequest_.reset();
        setUnlockedLocked(std::move(*fulfilled));
        return true;
    }

    // Drops the local license and any pending activation request.
    void clearLicense()
    {
        ++validationGeneration_; // invalidate any in-flight async revalidation

        try { licensing_->store().delete_local_license(); }
        catch (const moonbase::storage_error&) {}

        const juce::ScopedLock lock(stateLock_);
        pendingRequest_.reset();
        setUnlockedLocked(std::nullopt);
    }

    // Returns true when a valid Moonbase license is loaded. Equivalent to the
    // inherited isUnlocked() (which is kept in sync); use whichever reads
    // better at the call site.
    [[nodiscard]] bool isMoonbaseUnlocked() const noexcept
    {
        const juce::ScopedLock lock(stateLock_);
        return current_.has_value();
    }

    [[nodiscard]] std::optional<moonbase::license> moonbaseLicense() const
    {
        const juce::ScopedLock lock(stateLock_);
        return current_;
    }

    [[nodiscard]] moonbase::licensing& licensing() noexcept { return *licensing_; }
    [[nodiscard]] const moonbase::licensing& licensing() const noexcept { return *licensing_; }

    // ---- juce::OnlineUnlockStatus overrides ---------------------------------

    juce::String getProductID() override
    {
        return juce::String(productId_);
    }

    bool doesProductIDMatch(const juce::String& returnedIDFromServer) override
    {
        return returnedIDFromServer.equalsIgnoreCase(juce::String(productId_));
    }

    // Returns the process-local RSA public key used to verify the synthetic
    // JUCE keyfiles we apply ourselves. Moonbase's own RS256 JWT verification
    // happens inside moonbase::licensing via OpenSSL and is unrelated.
    juce::RSAKey getPublicKey() override
    {
        return juceUnlockPublicKey_;
    }

    void saveState(const juce::String& state) override
    {
        const juce::ScopedLock lock(stateLock_);
        opaqueJuceState_ = state;
    }

    juce::String getState() override
    {
        const juce::ScopedLock lock(stateLock_);
        return opaqueJuceState_;
    }

    juce::String getWebsiteName() override
    {
        return websiteName_;
    }

    // Not used in the Moonbase flow — activation URLs come from
    // beginActivation() instead. Reaching this means JUCE's email/password
    // unlock path was wired up by mistake (e.g. juce::OnlineUnlockForm).
    juce::URL getServerAuthenticationURL() override
    {
        jassertfalse;
        return juce::URL();
    }

    // Same caveat as above: JUCE only calls this from attemptWebserverUnlock,
    // which the Moonbase flow never invokes. Returning empty keeps the base
    // class's failure handling sane in release builds.
    juce::String readReplyFromWebserver(const juce::String& /*email*/,
                                        const juce::String& /*password*/) override
    {
        jassertfalse;
        return {};
    }

    // Returns a single-entry list with the moonbase device fingerprint, which
    // is also what we encode into the synthesized keyfiles. This keeps
    // applyKeyFile()'s machine-number match step consistent with our own
    // notion of device identity.
    juce::StringArray getLocalMachineIDs() override
    {
        return juce::StringArray(juce::String(licensing_->fingerprint().device_id()));
    }

private:
    void setUnlocked(std::optional<moonbase::license> license)
    {
        const juce::ScopedLock lock(stateLock_);
        setUnlockedLocked(std::move(license));
    }

    void setUnlockedLocked(std::optional<moonbase::license> license)
    {
        current_ = std::move(license);
        if (current_)
            applyLicenseToJuceState(*current_);
        else
            clearJuceState();
    }

    void applyLicenseToJuceState(const moonbase::license& lic)
    {
        const auto machineId = juce::String(licensing_->fingerprint().device_id());
        const auto appId = juce::String(productId_);
        const auto email = juce::String(lic.issued_to.email);
        const auto userName = lic.issued_to.name.empty()
            ? email
            : juce::String(lic.issued_to.name);

        // First pass: a non-expiring keyfile sets the unlocked flag. JUCE
        // never sets it for expiring keyfiles, so this baseline call is
        // required even when the license has an expiry.
        applyKeyFile(juce::KeyGeneration::generateKeyFile(
            appId, email, userName, machineId, juceUnlockPrivateKey_));

        // Second pass (only if expiring): JUCE preserves the previously-set
        // unlocked flag on this call and additionally records the expiry,
        // giving us both isUnlocked() == true and a non-zero getExpiryTime().
        if (lic.expires_at)
        {
            const auto expiryMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                lic.expires_at->time_since_epoch()).count();

            applyKeyFile(juce::KeyGeneration::generateExpiringKeyFile(
                appId, email, userName, machineId,
                juce::Time(static_cast<juce::int64>(expiryMs)),
                juceUnlockPrivateKey_));
        }
    }

    void clearJuceState()
    {
        // applyKeyFile() removes the unlocked flag at entry and only sets it
        // back if the keyfile's machine numbers match a local one. Synthesize
        // a well-formed keyfile pinned to a non-matching machine ID to force
        // a "no match" result and clear the flag.
        applyKeyFile(juce::KeyGeneration::generateKeyFile(
            juce::String(productId_),
            "moonbase-clear@invalid",
            "moonbase-clear",
            "moonbase-no-match",
            juceUnlockPrivateKey_));

        // Also wipe the email JUCE picked up from the synthetic keyfile so
        // that getUserEmail() doesn't expose our placeholder address.
        setUserEmail({});
    }

    std::string productId_;
    juce::String websiteName_;
    juce::String opaqueJuceState_;

    juce::RSAKey juceUnlockPublicKey_;
    juce::RSAKey juceUnlockPrivateKey_;

    mutable juce::CriticalSection stateLock_;
    std::optional<moonbase::license> current_;
    std::optional<moonbase::activation_request> pendingRequest_;

    std::shared_ptr<moonbase::licensing> licensing_;

    // Bumped by every state-mutating entry point (tryLoadStoredLicense*,
    // pollPendingActivation on success, clearLicense). Async continuations
    // capture the value at request time and skip both state mutation and
    // callback if the value has changed by the time they run.
    std::atomic<std::uint64_t> validationGeneration_{0};

    JUCE_DECLARE_WEAK_REFERENCEABLE(MoonbaseUnlockStatus)
};

} // namespace moonbase::juce_bridge
