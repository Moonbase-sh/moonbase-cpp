// Reference sample: a JUCE component that drives Moonbase activation end to
// end. Used by Main.cpp to populate the standalone example app, but you can
// drop it straight into a plugin GUI as well.
//
// The endpoint, product_id, and public key below are hardcoded to the public
// Moonbase demo (https://demo.moonbase.sh, product "demo-app"). Replace them
// with your own values when integrating into a real product.

#pragma once

#include "MoonbaseJuceBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

class PluginActivationComponent : public juce::Component,
                                  private juce::Timer
{
public:
    PluginActivationComponent()
        : unlockStatus_(makeOptions(), makeStore())
    {
        addAndMakeVisible(statusLabel_);
        statusLabel_.setJustificationType(juce::Justification::centredLeft);

        addAndMakeVisible(activateButton_);
        activateButton_.onClick = [this] { startActivation(); };

        addAndMakeVisible(deactivateButton_);
        deactivateButton_.onClick = [this] { unlockStatus_.clearLicense(); refreshLabel(); };

        // Async load: never blocks the message thread on libcurl. The label is
        // updated optimistically from the local-validation result, then again
        // from the message-thread callback once the online check resolves.
        unlockStatus_.tryLoadStoredLicenseAsync([this](auto) { refreshLabel(); });
        refreshLabel();

        setSize(520, 184);
    }

    ~PluginActivationComponent() override
    {
        stopTimer();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        statusLabel_.setBounds(area.removeFromTop(96));
        area.removeFromTop(8);
        auto buttons = area.removeFromTop(36);
        activateButton_.setBounds(buttons.removeFromLeft(200));
        buttons.removeFromLeft(12);
        deactivateButton_.setBounds(buttons.removeFromLeft(200));
    }

    [[nodiscard]] bool isUnlocked() const noexcept
    {
        // Either accessor works — they stay in sync. Showcasing the inherited
        // juce::OnlineUnlockStatus path here so existing plugin code that
        // takes an OnlineUnlockStatus& sees the right answer.
        return static_cast<bool>(unlockStatus_.isUnlocked());
    }

private:
    static moonbase::licensing_options makeOptions()
    {
        moonbase::licensing_options options;
        options.endpoint = "https://demo.moonbase.sh";
        options.product_id = "demo-app";
        options.public_key = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAutOqeUiPMgYjAwQ53CyKhJSqojr2bejce0CshQi9Hd8mNZbkoROx
oS56eIzehFSlX4YwHnF47AR1+fPOe7Q33Cgzd6d9xqksiMH7sWK2mADIlB66vZdW
uk3Me0UMB22Biy1RQbSRMivu79MxCofsympoL/5CFjJLd1u37kxjuRWVLjJS84Rr
3L2W7R7Exnno/giC+L/Dv711mjgstmtlAQm5ZINvFvoLA1eFTDs6nlCs3dpJSiq3
fsBUMT9FtudzS5As54jeT/8MB66fJJ0A1LQ/v5CW8ACQYseFSIoOKErD3xU7QLIJ
ERUn++6CVMPvZo67jVbTY+GCXYfW4gGVZQIDAQAB
-----END RSA PUBLIC KEY-----)";

        moonbase::juce_bridge::applyJuceMetadata(options);
        return options;
    }

    // Persists the validated license under the platform's per-user app data
    // directory (~/Library/Application Support/MoonbaseJuceExample on macOS,
    // %APPDATA%\MoonbaseJuceExample on Windows, ~/.config/MoonbaseJuceExample
    // on Linux). Replace "MoonbaseJuceExample" with your own product name in
    // a real integration.
    static std::shared_ptr<moonbase::file_license_store> makeStore()
    {
        const auto path = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                              .getChildFile("MoonbaseJuceExample")
                              .getChildFile("license.mb")
                              .getFullPathName()
                              .toStdString();
        return std::make_shared<moonbase::file_license_store>(path);
    }

    void startActivation()
    {
        try
        {
            const auto url = unlockStatus_.beginActivation();
            url.launchInDefaultBrowser();
            startTimer(1000);
            refreshLabel("Waiting for activation in browser...");
        }
        catch (const std::exception& ex)
        {
            refreshLabel(juce::String("Activation request failed: ") + ex.what());
        }
    }

    void timerCallback() override
    {
        if (unlockStatus_.pollPendingActivation())
        {
            stopTimer();
            refreshLabel();
        }
    }

    void refreshLabel(const juce::String& explicitMessage = {})
    {
        if (explicitMessage.isNotEmpty())
        {
            statusLabel_.setText(explicitMessage, juce::dontSendNotification);
            return;
        }

        if (auto license = unlockStatus_.moonbaseLicense())
        {
            juce::String text;
            text << "Unlocked as " << juce::String(license->issued_to.email);
            if (license->trial)
                text << "  (trial)";

            // getExpiryTime() comes from juce::OnlineUnlockStatus and is
            // populated from the Moonbase license's expires_at by the bridge.
            const auto expiry = unlockStatus_.getExpiryTime();
            if (expiry.toMilliseconds() > 0)
                text << "\nExpires " << expiry.toString(true, true);

            // Last successful validation — refreshed by validate_token_online
            // (or the local-only fallback within grace) and persisted into the
            // license's `validated_at` claim.
            const auto validatedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                license->validated_at.time_since_epoch()).count();
            text << "\nLast validated " << juce::Time(validatedMs).toString(true, true);

            statusLabel_.setText(text, juce::dontSendNotification);
        }
        else
        {
            statusLabel_.setText("Not activated", juce::dontSendNotification);
        }
    }

    moonbase::juce_bridge::MoonbaseUnlockStatus unlockStatus_;

    juce::Label statusLabel_;
    juce::TextButton activateButton_ { "Activate..." };
    juce::TextButton deactivateButton_ { "Deactivate" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginActivationComponent)
};
