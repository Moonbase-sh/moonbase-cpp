// Standalone sample app for the moonbase_licensing JUCE module.
//
// It mimics a real plugin editor ("Solstice") with a License button, and shows
// the activation flow as a MODAL OVERLAY on top of it. "Open Solstice", the
// close button, and a successful activation all just dismiss the overlay to
// reveal the app underneath; the License button brings it back. The endpoint /
// product id / public key are the public Moonbase demo values.

#include <moonbase_licensing/moonbase_licensing.h>

#include <memory>
#include <vector>

using moonbase::juce_integration::ActivationComponent;
using moonbase::juce_integration::ActivationConfig;
using Screen = moonbase::juce_integration::ActivationController::Screen;

static ActivationConfig makeConfig()
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";
    config.publicKey = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAutOqeUiPMgYjAwQ53CyKhJSqojr2bejce0CshQi9Hd8mNZbkoROx
oS56eIzehFSlX4YwHnF47AR1+fPOe7Q33Cgzd6d9xqksiMH7sWK2mADIlB66vZdW
uk3Me0UMB22Biy1RQbSRMivu79MxCofsympoL/5CFjJLd1u37kxjuRWVLjJS84Rr
3L2W7R7Exnno/giC+L/Dv711mjgstmtlAQm5ZINvFvoLA1eFTDs6nlCs3dpJSiq3
fsBUMT9FtudzS5As54jeT/8MB66fJJ0A1LQ/v5CW8ACQYseFSIoOKErD3xU7QLIJ
ERUn++6CVMPvZo67jVbTY+GCXYfW4gGVZQIDAQAB
-----END RSA PUBLIC KEY-----)";

    config.productName = "Solstice";
    config.manufacturerName = "Helio Audio";
    config.accent = juce::Colour(0xff186cdc);
    config.enableTrial = false;     // online activation only
    config.overlayBackdrop = true;  // render as a modal over the plugin editor
    config.applicationVersion = JUCE_APPLICATION_VERSION_STRING;

    // Telemetry: attach JUCE system/host metadata to activation requests.
    config.analytics.enabled = true;
    config.metadata["app.channel"] = "sample"; // a custom field, sent alongside
    config.onDiagnostic = [] (const juce::String& message) {
        juce::Logger::writeToLog("[activation] " + message);
    };
    return config;
}

//==============================================================================
// A stand-in plugin editor. The real point of the sample is the activation
// modal it hosts; the knobs are just so there's an app to reveal.
class PluginEditor : public juce::Component
{
public:
    PluginEditor()
    {
        const char* knobNames[] = { "Drive", "Warmth", "Mix", "Output" };
        for (int i = 0; i < 4; ++i)
        {
            auto knob = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag,
                                                       juce::Slider::NoTextBox);
            knob->setRange(0.0, 1.0);
            knob->setValue(0.25 + 0.18 * i);
            knob->setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff186cdc));
            knob->setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0x22ffffff));
            knob->setColour(juce::Slider::thumbColourId, juce::Colour(0xfff5f8fb));
            addAndMakeVisible(*knob);

            auto label = std::make_unique<juce::Label>(juce::String(), knobNames[i]);
            label->setJustificationType(juce::Justification::centred);
            label->setColour(juce::Label::textColourId, juce::Colour(0xff90a0b8));
            label->setFont(juce::FontOptions(12.5f));
            addAndMakeVisible(*label);

            knobs.push_back(std::move(knob));
            labels.push_back(std::move(label));
        }

        licenseButton.setButtonText("License");
        licenseButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0x18ffffff));
        licenseButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffcdd8e6));
        licenseButton.onClick = [this] { showActivation(); };
        addAndMakeVisible(licenseButton);

        activation = std::make_unique<ActivationComponent>(makeConfig());
        activation->onClose = [this] { hideActivation(); }; // "Open", close (X), success all dismiss
        activation->onActivationChanged = [this](bool activated)
        {
            // On launch, lock behind the modal only if not already licensed.
            // Once the initial check settles, the License button drives it.
            if (! initialCheckSettled
                && activation->controller().screen() != Screen::Loading)
            {
                initialCheckSettled = true;
                if (! activated)
                    showActivation();
            }
        };
        addChildComponent(*activation);
        setSize(760, 520);
    }

    void showActivation() { activation->appear(); }
    void hideActivation() { activation->dismiss(); }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        juce::ColourGradient bg(juce::Colour(0xff151c29), b.getCentreX(), 0.0f,
                                juce::Colour(0xff090d15), b.getCentreX(), b.getHeight(), false);
        g.setGradientFill(bg);
        g.fillRect(b);

        // top bar
        auto header = getLocalBounds().removeFromTop(96).reduced(28, 0);
        g.setColour(juce::Colour(0xfff5f8fb));
        g.setFont(juce::FontOptions(30.0f, juce::Font::bold));
        g.drawText("SOLSTICE", header.removeFromTop(64), juce::Justification::bottomLeft);
        g.setColour(juce::Colour(0xff768aa4));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("Saturator  by  Helio Audio", header, juce::Justification::topLeft);

        g.setColour(juce::Colour(0x12ffffff));
        g.fillRect(getLocalBounds().withTrimmedTop(96).removeFromTop(1).reduced(28, 0));
    }

    void resized() override
    {
        activation->setBounds(getLocalBounds());
        licenseButton.setBounds(getWidth() - 28 - 96, 30, 96, 32);

        auto row = getLocalBounds().withTrimmedTop(150).reduced(40, 0).removeFromTop(170);
        const int cell = row.getWidth() / juce::jmax(1, (int) knobs.size());
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            auto c = row.removeFromLeft(cell);
            knobs[i]->setBounds(c.removeFromTop(128).reduced(14));
            labels[i]->setBounds(c.removeFromTop(22));
        }
    }

private:
    std::vector<std::unique_ptr<juce::Slider>> knobs;
    std::vector<std::unique_ptr<juce::Label>> labels;
    juce::TextButton licenseButton;
    std::unique_ptr<ActivationComponent> activation;
    bool initialCheckSettled = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

//==============================================================================
class MoonbaseActivationApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Moonbase Activation"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String&) override { mainWindow.reset(new MainWindow()); }
    void shutdown() override                       { mainWindow = nullptr; }
    void systemRequestedQuit() override            { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow()
            : juce::DocumentWindow("Solstice", juce::Colour(0xff090d15),
                                   juce::DocumentWindow::allButtons)
        {
            auto* editor = new PluginEditor();
            setUsingNativeTitleBar(true);
            setContentOwned(editor, true);
            setResizable(true, false);
            // Deliberately allow sizes below the modal's minimum so you can see
            // the activation modal scale down to fit a small host window.
            setResizeLimits(360, 320, 1400, 1000);
            centreWithSize(editor->getWidth(), editor->getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(MoonbaseActivationApplication)
