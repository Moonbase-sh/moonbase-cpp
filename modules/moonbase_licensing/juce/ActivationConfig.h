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

#include <moonbase/moonbase.hpp>

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "JuceMetadata.h"

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
    juce::URL manageUrl  = juce::URL("https://moonbase.sh");
    juce::URL supportUrl;

    // Where the customer exchanges their machine file for a license file during
    // offline activation. Defaults to "{endpoint}/activate" when left unset.
    juce::URL activationUrl;

    bool showMoonbaseBadge = true;
    bool enableOffline = true;     // show the offline activation flow
    bool reduceMotion = false;     // skip transition/spinner/pop animation (a11y + snapshot tests)
    bool overlayBackdrop = false;  // dim the host behind the panel (modal over a plugin) instead of a full opaque backdrop
    int trialLengthDays = 14;      // trial length shown on the Trial / Expired screens (trials are granted by the backend, not started from the UI)

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
