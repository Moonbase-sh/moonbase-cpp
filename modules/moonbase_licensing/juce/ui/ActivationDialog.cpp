#include "ActivationDialog.h"

namespace moonbase::juce_integration {

namespace {

class ActivationDialogWindow : public juce::DocumentWindow
{
public:
    ActivationDialogWindow(ActivationConfig config, std::function<void(bool)> onClosedIn)
        : juce::DocumentWindow("Activate " + config.resolvedProductName(),
                               juce::Colour(0xff04060b), juce::DocumentWindow::closeButton),
          onClosed(std::move(onClosedIn))
    {
        auto* comp = new ActivationComponent(std::move(config));
        comp->onClose = [this] { closeButtonPressed(); };

        setUsingNativeTitleBar(true);
        setContentOwned(comp, true);
        setResizable(false, false);
        centreWithSize(comp->getWidth(), comp->getHeight());
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        bool activated = false;
        if (auto* comp = dynamic_cast<ActivationComponent*>(getContentComponent()))
            activated = comp->controller().license().has_value();
        if (onClosed)
            onClosed(activated);
        delete this; // self-owned
    }

private:
    std::function<void(bool)> onClosed;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivationDialogWindow)
};

} // namespace

void ActivationDialog::show(ActivationConfig config, std::function<void(bool)> onClosed)
{
    new ActivationDialogWindow(std::move(config), std::move(onClosed));
}

} // namespace moonbase::juce_integration
