// JUCEApplication entry point for the Moonbase JUCE example. Builds a tiny
// desktop window that hosts PluginActivationComponent.

#include "PluginActivationComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

class MoonbaseJuceExampleApplication : public juce::JUCEApplication
{
public:
    MoonbaseJuceExampleApplication() = default;

    const juce::String getApplicationName() override        { return "Moonbase JUCE Example"; }
    const juce::String getApplicationVersion() override     { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override              { return true; }

    void initialise(const juce::String&) override
    {
        mainWindow_.reset(new MainWindow(getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow_ = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : juce::DocumentWindow(
                  name,
                  juce::Desktop::getInstance().getDefaultLookAndFeel()
                      .findColour(juce::ResizableWindow::backgroundColourId),
                  juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new PluginActivationComponent(), true);
            setResizable(true, false);
            centreWithSize(getWidth(), getHeight());
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
    std::unique_ptr<MainWindow> mainWindow_;
};

START_JUCE_APPLICATION(MoonbaseJuceExampleApplication)
