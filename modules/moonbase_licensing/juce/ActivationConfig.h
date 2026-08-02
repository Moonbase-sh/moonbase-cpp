#pragma once

// Everything needed to wire up + brand an activation flow. The connection
// fields configure the Moonbase SDK; the branding fields drive the built-in UI
// (the Solstice design's product name, accent, trial copy, co-brand badge).

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__)
 #include <TargetConditionals.h> // TARGET_OS_IPHONE, for the iOS resolver default
#endif

#include <moonbase/moonbase.hpp>

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "JuceMetadata.h"
// Both are named by resolvedDeviceIdResolver(). Included here rather than relied
// on from the module umbrella so this header stays usable on its own.
#include "android_device_id_resolver.h"
#include "ios_device_id_resolver.h"
#include "legacy_juce_device_id_resolver.h"

// Normally defined by the module umbrella header; fall back so this header is
// self-contained if included on its own.
#ifndef MOONBASE_LICENSING_VERSION
 #define MOONBASE_LICENSING_VERSION "0.0.0"
#endif

namespace moonbase::juce_integration {

struct TrialFeature
{
    juce::String label;
    bool included = true; // true -> green check, false -> dimmed cross
};

// Merchant-overridable UI copy. Leave any field empty to use the built-in
// default (several interpolate productName / manufacturerName). Override only what
// you want to change, e.g. config.strings.welcomeBody = "...".
struct ActivationStrings
{
    juce::String welcomeTitle;     // default: "Activate {productName}"
    juce::String welcomeBody;      // default: "Unlock the full plugin through your {manufacturerName} account."
    juce::String activateOnline;   // default: "Activate online"
    juce::String activateOffline;  // default: "No internet? Activate offline"
};

struct ActivationConfig
{
    //== Connection (Moonbase SDK) =============================================
    juce::String endpoint;       // e.g. "https://your-tenant.moonbase.sh"
    juce::String productId;      // e.g. "your-product"
    juce::String publicKey;      // RSA public key (PEM or base64 DER)
    juce::String accountId;      // optional issuer pin
    juce::String applicationVersion;

    //== Validation / network tuning ==========================================
    // How long a license stays valid offline since its last successful online
    // validation before it is treated as stale (and the app locks). Default 7 days.
    std::chrono::seconds onlineGracePeriod{std::chrono::hours(24 * 7)};

    // Minimum time between online validation calls. Within this window a
    // validation returns the cached license with no network round trip, so this
    // is the effective "how often we re-check online" cadence. Default 5 minutes.
    // (controller().refreshLicense(force=true) bypasses it.)
    std::chrono::seconds onlineCheckInterval{std::chrono::minutes(5)};

    // HTTP timeouts for activation / validation requests (they run on background
    // threads, so these never block the UI). Defaults: 10s connect, 30s request.
    std::chrono::milliseconds httpConnectTimeout{std::chrono::seconds(10)};
    std::chrono::milliseconds httpRequestTimeout{std::chrono::seconds(30)};

    // Where the validated license is persisted. Defaults to a per-user app-data
    // file under "<manufacturer>/<product>/license.mb" when left empty
    // (see resolvedLicenseFile()).
    juce::File licenseFile;

    //== Branding / UI =========================================================
    juce::String productName;       // defaults to JucePlugin_Name (see resolvedProductName())
    juce::String manufacturerName;  // defaults to JucePlugin_Manufacturer; shown under the product name
    juce::Colour accent = juce::Colour(0xff186cdc);        // Moonbase blue

    // Where the customer exchanges their machine file for a license file during
    // offline activation. Defaults to "{endpoint}/activate" when left unset.
    juce::URL activationUrl;

    bool showMoonbaseBadge = true;
    bool enableOffline = true;     // show the offline activation flow
    bool reduceMotion = false;     // skip transition/spinner/pop animation (a11y + snapshot tests)
    bool overlayBackdrop = false;  // dim the host behind the panel (modal over a plugin) instead of a full opaque backdrop
    int trialLengthDays = 14;      // trial length shown on the Trial / Expired screens (trials are granted by the backend, not started from the UI)

    // When a validated license reports a newer released version than
    // applicationVersion (the p:rel claim), show the "Update available" screen and
    // offer an in-app download of the new installer. Set false to never prompt.
    bool enableUpdatePrompt = true;

    // Automatically present the update screen when the plugin opens with an
    // available (non-skipped) update. Set false to surface updates only via the
    // "Update available" badge in the license view (no automatic pop on open).
    bool autoPresentUpdate = true;

    // Where the downloaded installer is saved. Defaults to the user's Downloads
    // folder (falling back to the temp directory) when left empty; see
    // resolvedDownloadDirectory().
    juce::File downloadDirectory;

    //==============================================================================
    // Device identity.

    // Overrides how this machine is identified to Moonbase. Leave null for the
    // default moonbase::moonbase_device_id_resolver, which implements the
    // cross-SDK device fingerprint spec, so a license activated in a web or
    // Electron app built on @moonbase.sh/licensing validates here too.
    //
    // Set it to a moonbase::migrating_device_id_resolver when upgrading a plugin
    // that already has activated users: the device id changed in 4.0.0, so
    // without one every existing install must re-activate, which consumes a fresh
    // activation seat and resets any device-scoped trial. See "Migrating from
    // 3.x" in the README.
    std::shared_ptr<moonbase::device_id_resolver> deviceIdResolver;

    // Accept a deliberately weaker device id (a hash of the host name, stamped
    // mbd2n_) on machines with no readable hardware identity, instead of failing
    // activation with moonbase::insufficient_device_identity_error.
    //
    // Off by default, because a host name is user-renameable, frequently
    // duplicated across imaged machines, and regenerated on every container
    // start. Ignored when deviceIdResolver is set.
    bool allowDeviceNameFallback = false;

    // Optional override for the device name shown on the activation screen (the
    // "<name>  ·  <platform>" chip). When empty, the resolver's own label is
    // used: the OS host name, with a trailing ".local" removed on macOS.
    juce::String deviceName;

    // Product / manufacturer brand mark shown in the header lockup. When unset,
    // a generated sun mark in the accent colour is drawn. Provide a vector
    // Drawable (e.g. juce::Drawable::createFromSVG(...)) for crispness:
    //   config.logo = std::shared_ptr<juce::Drawable>(juce::Drawable::createFromSVG(*xml));
    std::shared_ptr<juce::Drawable> logo;

    // Optional UI copy overrides (see ActivationStrings).
    ActivationStrings strings;

    // Optional feature list shown on the trial screen (included vs. excluded).
    std::vector<TrialFeature> trialFeatures;

    // Optional sink for diagnostic detail (the underlying error behind the
    // friendly UI text). Wire it to juce::Logger, a file, or your telemetry to
    // debug activation issues in the field. Invoked on the message thread.
    std::function<void(const juce::String& message)> onDiagnostic;

    //== Telemetry / analytics =================================================
    // Off by default. Set analytics.enabled = true to attach JUCE system/host
    // metadata (OS, CPU, JUCE version, DAW host, plugin format, ...) to every
    // activation + validation request. See JuceMetadata.h / AnalyticsOptions.
    AnalyticsOptions analytics;

    // Extra metadata sent with every request. Merged first, so these win over
    // the auto-collected analytics keys on a collision.
    std::map<std::string, std::string> metadata;

    // Last-word hook to add or rewrite metadata programmatically (e.g. a build
    // channel, an A/B cohort). Receives the assembled map; runs after the
    // static metadata + analytics capture.
    std::function<void(std::map<std::string, std::string>&)> onCollectMetadata;

    //== Resolved display names ================================================
    // The product / manufacturer shown throughout the UI and used for the
    // default license path. Prefer the explicit config field; otherwise fall
    // back to the JUCE plugin macros (so a plugin target "just works" without
    // duplicating its name here), then to a neutral default. Route every
    // display-name read through these so branding stays consistent everywhere.
    [[nodiscard]] juce::String resolvedProductName() const
    {
        if (productName.isNotEmpty())
            return productName;
       #if defined (JucePlugin_Name)
        return JucePlugin_Name;
       #else
        return "Your Plugin";
       #endif
    }
    [[nodiscard]] juce::String resolvedManufacturerName() const
    {
        if (manufacturerName.isNotEmpty())
            return manufacturerName;
       #if defined (JucePlugin_Manufacturer)
        return JucePlugin_Manufacturer;
       #else
        return {};
       #endif
    }
    // The running app version compared against the license's released-version
    // claim. Mirrors the resolution in toLicensingOptions(): explicit config, then
    // the JUCE plugin macro. Empty when neither is set (no update prompt then).
    [[nodiscard]] juce::String resolvedApplicationVersion() const
    {
        if (applicationVersion.isNotEmpty())
            return applicationVersion;
       #if defined (JucePlugin_VersionString)
        return JucePlugin_VersionString;
       #else
        return {};
       #endif
    }

    //== Resolved UI copy (override-or-default) ================================
    [[nodiscard]] juce::String brandAccountName() const
    {
        const auto manufacturer = resolvedManufacturerName();
        return manufacturer.isNotEmpty() ? manufacturer : juce::String("Moonbase");
    }
    [[nodiscard]] juce::String welcomeTitleText() const
    {
        return strings.welcomeTitle.isNotEmpty() ? strings.welcomeTitle
                                                  : juce::String("Activate ") + resolvedProductName();
    }
    [[nodiscard]] juce::String welcomeBodyText() const
    {
        if (strings.welcomeBody.isNotEmpty())
            return strings.welcomeBody;
        return "Unlock the full plugin through your " + brandAccountName() + " account.";
    }
    [[nodiscard]] juce::String activateOnlineText() const
    {
        return strings.activateOnline.isNotEmpty() ? strings.activateOnline
                                                   : juce::String("Activate online");
    }
    [[nodiscard]] juce::String activateOfflineText() const
    {
        return strings.activateOffline.isNotEmpty() ? strings.activateOffline
                                                    : juce::String("No internet? Activate offline");
    }
    [[nodiscard]] juce::URL activationUrlResolved() const
    {
        if (activationUrl.toString(false).isNotEmpty())
            return activationUrl;
        return juce::URL(endpoint.trimCharactersAtEnd("/") + "/activate");
    }
    [[nodiscard]] juce::String activationUrlDisplay() const
    {
        // Host + path without the scheme, e.g. "your-tenant.moonbase.sh/activate".
        return activationUrlResolved().toString(false).fromFirstOccurrenceOf("://", false, false);
    }

    //== Helpers ===============================================================
    // Returns an empty string when the connection fields are well-formed, or a
    // human-readable reason when they're not. The controller surfaces this as an
    // Error state rather than constructing the SDK with a broken configuration.
    [[nodiscard]] juce::String validate() const
    {
        if (endpoint.trim().isEmpty())
            return "No activation endpoint configured (set config.endpoint).";
        if (! endpoint.trim().startsWithIgnoreCase("http"))
            return "Activation endpoint must be an http(s) URL (set config.endpoint).";
        if (productId.trim().isEmpty())
            return "No product id configured (set config.productId).";
        if (publicKey.trim().isEmpty())
            return "No public key configured (set config.publicKey).";
        return {};
    }

    [[nodiscard]] juce::File resolvedLicenseFile() const
    {
        if (licenseFile != juce::File())
            return licenseFile;

        const auto resolvedManufacturer = resolvedManufacturerName();
        const auto manufacturer = resolvedManufacturer.isNotEmpty() ? resolvedManufacturer : juce::String("Moonbase");
        const auto product = resolvedProductName();
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile(manufacturer)
            .getChildFile(product)
            .getChildFile("license.mb");
    }

    [[nodiscard]] juce::File resolvedDownloadDirectory() const
    {
        if (downloadDirectory != juce::File())
            return downloadDirectory;

        auto downloads = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                             .getChildFile("Downloads");
        if (downloads.isDirectory())
            return downloads;
        return juce::File::getSpecialLocation(juce::File::tempDirectory);
    }

    // The resolver the controller will use.
    [[nodiscard]] std::shared_ptr<moonbase::device_id_resolver> resolvedDeviceIdResolver() const
    {
        if (deviceIdResolver != nullptr)
            return deviceIdResolver;

        return defaultDeviceIdResolver(allowDeviceNameFallback);
    }

    /// The resolver this platform gets when `deviceIdResolver` is left unset.
    ///
    /// Static and public because a migrating configuration must wrap *this*, not a
    /// hard-coded `moonbase_device_id_resolver`. On iOS and Android the spec
    /// resolver has no identity parameters to read and throws, and a migrating
    /// wrapper asks its current resolver for an id before consulting any historical
    /// one, so hard-coding it there locks mobile users out of both validation and
    /// new activation:
    ///
    /// \code
    /// config.deviceIdResolver = std::make_shared<moonbase::migrating_device_id_resolver>(
    ///     ActivationConfig::defaultDeviceIdResolver(),
    ///     std::make_shared<legacy_juce_device_id_resolver>());
    /// \endcode
    [[nodiscard]] static std::shared_ptr<moonbase::device_id_resolver> defaultDeviceIdResolver(
        bool allowHostNameFallback = false)
    {
        // A constexpr flag with an ordinary `if`, rather than an #if around the
        // returns: in a non-template function both branches are still compiled, so
        // the mobile paths cannot rot unnoticed on a desktop-only CI matrix.
        if (isAndroid)
        {
            // Android's only device identifier is scoped to the app signing key, so
            // the id is scoped and stamped mbd2s_. Deliberately not
            // juce::SystemStats::getUniqueDeviceID(): it hashes the string constant
            // "android_id" rather than the device's value, so every JUCE Android app
            // reports the same id. See android_device_id_resolver.h.
            //
            // allowHostNameFallback is ignored, as it is on iOS: the label is
            // Build.MODEL on most devices, so hashing it would give a whole product
            // line one id. build_fingerprint_material refuses it outright.
            return std::make_shared<android_device_id_resolver>();
        }

        if (isMobileApple)
        {
            // iOS gets a *scoped* spec identity, stamped mbd2s_.
            //
            // iOS exposes no device identifier that unrelated applications can
            // read: no hardware serial since iOS 7, and identifierForVendor is
            // scoped to the App Store vendor. Cross-SDK parity is therefore
            // impossible by construction, and the stamp says so rather than
            // pretending otherwise.
            //
            // Note this excludes Mac Catalyst, which runs on macOS, can read IOKit,
            // and so takes the cross-SDK hardware identity below.
            return std::make_shared<ios_device_id_resolver>();
        }

        moonbase::moonbase_device_id_resolver_options options;
        options.fallback = allowHostNameFallback
            ? moonbase::device_id_fallback::device_name
            : moonbase::device_id_fallback::none;
        return std::make_shared<moonbase::moonbase_device_id_resolver>(std::move(options));
    }

    // Platforms whose only device identifier is scoped, so the default resolver is
    // a scoped one rather than the cross-SDK hardware fingerprint. Exposed so a
    // consumer (and the module's tests) can reason about which default they get
    // without duplicating the platform test.
    // True on a real iOS/tvOS/watchOS device, and on an unmodified iOS app running
    // on Apple silicon. Deliberately NOT Mac Catalyst: TARGET_OS_IPHONE is 1 there
    // too, but a Catalyst build runs on macOS and can read IOKit, so it takes the
    // cross-SDK hardware identity. See platform_tag() in fingerprint_spec.hpp.
    static constexpr bool isMobileApple =
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
        true;
#else
        false;
#endif

    static constexpr bool isAndroid =
#if defined(__ANDROID__) || JUCE_ANDROID
        true;
#else
        false;
#endif

    /// True where the device id is scoped, and therefore not cross-SDK comparable.
    static constexpr bool hasScopedIdentityOnly = isMobileApple || isAndroid;

    [[nodiscard]] moonbase::licensing_options toLicensingOptions() const
    {
        moonbase::licensing_options options;
        options.endpoint = endpoint.toStdString();
        options.product_id = productId.toStdString();
        options.public_key = publicKey.toStdString();
        if (accountId.isNotEmpty())
            options.account_id = accountId.toStdString();
        if (applicationVersion.isNotEmpty())
            options.application_version = applicationVersion.toStdString();
       #if defined (JucePlugin_VersionString)
        else
            options.application_version = JucePlugin_VersionString; // a plugin has no JUCEApplication to read it from
       #endif

        // Identify this client as the JUCE module (appended to the base client's
        // User-Agent), with the JUCE version + OS for support/analytics.
        juce::String clientInfo;
        clientInfo << "moonbase-juce/" << MOONBASE_LICENSING_VERSION
                   << " (" << juce::SystemStats::getJUCEVersion()
                   << "; " << juce::SystemStats::getOperatingSystemName() << ")";
        options.client_info = clientInfo.toStdString();

        options.online_validation_grace_period = onlineGracePeriod;
        options.online_validation_min_interval = onlineCheckInterval;
        options.http_connect_timeout = httpConnectTimeout;
        options.http_request_timeout = httpRequestTimeout;

        // Explicit metadata first (so it wins on key collisions), then the
        // opt-in JUCE analytics capture, then the caller's last-word hook.
        for (const auto& entry : metadata)
            options.metadata.emplace(entry.first, entry.second);
        if (analytics.enabled)
            applyJuceMetadata(options, analytics);
        if (onCollectMetadata)
            onCollectMetadata(options.metadata);

        return options;
    }
};

} // namespace moonbase::juce_integration
