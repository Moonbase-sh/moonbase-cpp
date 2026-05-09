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
          licensing_(std::move(options), std::move(store), std::move(fingerprint))
    {
        juce::RSAKey::createKeyPair(juceUnlockPublicKey_, juceUnlockPrivateKey_, 512);
    }

    // ---- Moonbase-facing API -------------------------------------------------

    // Loads any previously stored license and validates its JWT. Call this once
    // from your AudioProcessor constructor (or app startup). Safe to call again
    // after reconfiguring the license store.
    bool tryLoadStoredLicense()
    {
        try
        {
            auto stored = licensing_.store().load_local_license();
            if (!stored)
            {
                setUnlocked(std::nullopt);
                return false;
            }

            auto validated = licensing_.validate_token_local(stored->token);
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
    }

    // Begins a new browser activation. Returns the URL to hand to
    // juce::URL::launchInDefaultBrowser. Throws moonbase::api_error on
    // network/server failure.
    juce::URL beginActivation()
    {
        const juce::ScopedLock lock(stateLock_);
        pendingRequest_ = licensing_.request_activation();
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

        auto fulfilled = licensing_.get_requested_activation(*request);
        if (!fulfilled)
            return false;

        try
        {
            licensing_.store().store_local_license(*fulfilled);
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
        try { licensing_.store().delete_local_license(); }
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

    [[nodiscard]] moonbase::licensing& licensing() noexcept { return licensing_; }
    [[nodiscard]] const moonbase::licensing& licensing() const noexcept { return licensing_; }

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
        return juce::StringArray(juce::String(licensing_.fingerprint().device_id()));
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
        const auto machineId = juce::String(licensing_.fingerprint().device_id());
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

    moonbase::licensing licensing_;
};

} // namespace moonbase::juce_bridge
