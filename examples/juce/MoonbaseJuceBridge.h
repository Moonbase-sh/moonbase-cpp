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

#include <chrono>
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

    // Optimistic, non-blocking variant of tryLoadStoredLicense. Behaviour:
    //   1. Loads the stored license and runs local validation synchronously on
    //      the calling thread (typically the message thread). Plugin/app is
    //      "unlocked" immediately if the cached token is locally valid.
    //   2. If the token is online-activated, kicks off the online check on a
    //      background thread. The result is marshalled back to the message
    //      thread, applied to the bridge state, and delivered via onComplete.
    //   3. If the bridge is destroyed while the background check is in flight,
    //      the callback is silently dropped — the destructor invalidates the
    //      weak reference the message-thread continuation captures.
    //
    // Call from your AudioProcessor constructor to avoid blocking the host's
    // plugin-load thread on libcurl. The grace period in licensing_options
    // governs what happens when the API is unreachable: within grace, the
    // optimistic local state is preserved (Tolerated); beyond grace, the
    // bridge is locked (Unreachable).
    //
    // The callback runs on the JUCE message thread; it's safe to touch UI
    // state inside it.
    void tryLoadStoredLicenseAsync(std::function<void(AsyncValidationResult)> onComplete = {})
    {
        std::optional<moonbase::license> local;
        try
        {
            auto stored = licensing_->store().load_local_license();
            if (!stored)
            {
                setUnlocked(std::nullopt);
                if (onComplete)
                    onComplete({AsyncValidationOutcome::NoStoredLicense, std::nullopt});
                return;
            }

            local = licensing_->validate_token_local(stored->token);
            setUnlocked(*local);
        }
        catch (const std::exception&)
        {
            setUnlocked(std::nullopt);
            if (onComplete)
                onComplete({AsyncValidationOutcome::LocalInvalid, std::nullopt});
            return;
        }

        if (local->method == moonbase::activation_method::offline)
        {
            if (onComplete)
                onComplete({AsyncValidationOutcome::OfflineToken, local});
            return;
        }

        // Capture by value: the licensing instance survives bridge destruction
        // via shared_ptr, and the weak reference protects the message-thread
        // continuation from touching a destroyed bridge.
        auto licensingHandle = licensing_;
        const auto token = local->token;
        juce::WeakReference<MoonbaseUnlockStatus> safeThis(this);
        auto cb = std::move(onComplete);

        juce::Thread::launch(
            [licensingHandle, token, safeThis, cb = std::move(cb)]() mutable
            {
                AsyncValidationResult result{AsyncValidationOutcome::Refreshed, std::nullopt};
                try
                {
                    auto refreshed = licensingHandle->validate_token_online(token);
                    result = {AsyncValidationOutcome::Refreshed, std::move(refreshed)};
                }
                catch (const moonbase::license_invalid_error&)
                {
                    result = {AsyncValidationOutcome::LockedInvalid, std::nullopt};
                }
                catch (const moonbase::license_expired_error&)
                {
                    result = {AsyncValidationOutcome::LockedExpired, std::nullopt};
                }
                catch (const std::exception&)
                {
                    // validate_token_online only throws non-license exceptions
                    // when grace has elapsed, so this is the past-grace case.
                    result = {AsyncValidationOutcome::Unreachable, std::nullopt};
                }

                juce::MessageManager::callAsync(
                    [safeThis, result = std::move(result), cb = std::move(cb)]() mutable
                    {
                        auto* self = safeThis.get();
                        if (self == nullptr)
                            return;

                        switch (result.outcome)
                        {
                        case AsyncValidationOutcome::Refreshed:
                            if (result.license)
                            {
                                try { self->licensing_->store().store_local_license(*result.license); }
                                catch (const moonbase::storage_error&) {}
                                self->setUnlocked(*result.license);
                            }
                            break;
                        case AsyncValidationOutcome::LockedInvalid:
                        case AsyncValidationOutcome::LockedExpired:
                        case AsyncValidationOutcome::Unreachable:
                            self->setUnlocked(std::nullopt);
                            break;
                        case AsyncValidationOutcome::NoStoredLicense:
                        case AsyncValidationOutcome::LocalInvalid:
                        case AsyncValidationOutcome::OfflineToken:
                            // Unreachable here; handled synchronously above.
                            break;
                        }

                        if (cb)
                            cb(std::move(result));
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

    JUCE_DECLARE_WEAK_REFERENCEABLE(MoonbaseUnlockStatus)
};

} // namespace moonbase::juce_bridge
