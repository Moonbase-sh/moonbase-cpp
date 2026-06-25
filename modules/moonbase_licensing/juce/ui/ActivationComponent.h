#pragma once

// The built-in activation window. Construct it with an ActivationConfig and add
// it to a window (or use ActivationDialog). It owns an ActivationController,
// renders the Solstice-style flow, and drives every transition with JUCE 8's
// animation API. Native end to end — it talks to the moonbase::licensing API
// directly, not juce::OnlineUnlockStatus.

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../ActivationConfig.h"
#include "../ActivationController.h"

namespace moonbase::juce_integration {

class ActivationComponent : public juce::Component
{
public:
    // Builds and owns its own ActivationController from the config.
    explicit ActivationComponent(ActivationConfig config);

    // Shares a controller owned elsewhere (e.g. one living in your AudioProcessor
    // so audio-thread gating survives the editor's lifetime) — both processor and
    // editor then see one license, with no hand-rolled re-sync. The controller
    // must outlive this component, and the owner is responsible for calling
    // controller.start(); the component reflects its current state.
    explicit ActivationComponent(ActivationController& sharedController);

    ~ActivationComponent() override;

    // Called when the user dismisses the flow from a "done" state — the
    // Welcome "no thanks" is not offered, so this fires from Success ("Open …")
    // and Trial ("Continue"). ActivationDialog wires this to close the window.
    std::function<void()> onClose;

    // Fired whenever activation state settles (true once a valid license is
    // loaded). Handy for gating: enable your plugin when this reports true.
    std::function<void(bool isActivated)> onActivationChanged;

    [[nodiscard]] ActivationController& controller();

    // Show/hide as a modal with a smooth fade + scale of both the panel and the
    // backdrop. Use these (instead of setVisible) when overlaying a host app:
    //   appear()  -> setVisible(true) and animate in
    //   dismiss() -> animate out, then setVisible(false)
    // "Open", the close button, and a successful activation call onClose; wire
    // onClose to dismiss().
    void appear();
    void dismiss();

    // Preferred window size for this design.
    static constexpr int defaultWidth = 660;
    static constexpr int defaultHeight = 600;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivationComponent)
};

} // namespace moonbase::juce_integration
