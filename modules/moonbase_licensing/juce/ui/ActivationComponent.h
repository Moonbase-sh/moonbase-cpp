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

    // Called when the user dismisses the flow from a "done" state. The Welcome
    // "no thanks" is not offered, so this fires from Success ("Open {product}"),
    // Trial ("Continue"), the close button once a license is loaded, and "Skip
    // this update" on an auto-presented update. ActivationDialog wires this to
    // close the window.
    std::function<void()> onClose;

    // Fired once the activation state has settled (after the stored license has
    // loaded), then whenever the license itself changes: activated, refreshed
    // into a new token, picked up from another instance, revoked, expired or
    // cleared. Never for screen navigation, busy flips or download progress, so
    // it is a safe place to reload features. true once a valid license is
    // loaded. Assign it right after construction; the first report is deferred
    // until then.
    std::function<void(bool isActivated)> onActivationChanged;

    [[nodiscard]] ActivationController& controller();

    // Show/hide as a modal with a smooth fade + scale of both the panel and the
    // backdrop. Use these (instead of setVisible) when overlaying a host app:
    //   appear()  -> setVisible(true) and animate in
    //   dismiss() -> animate out, then setVisible(false)
    // A successful activation shows an "Open {product}" button rather than
    // closing by itself; that button and the close button call onClose, so wire
    // onClose to dismiss(). While hidden the panel runs no timers.
    void appear();
    void dismiss();

    // Present the update screen now if an update is available (ignores a previous
    // skip). The module already auto-presents on open when config.autoPresentUpdate
    // is set; this is exposed so a host can trigger it explicitly too. No-op otherwise.
    void presentUpdateIfAvailable();

    // Preferred window size for this design.
    static constexpr int defaultWidth = 660;
    static constexpr int defaultHeight = 600;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivationComponent)
};

} // namespace moonbase::juce_integration
