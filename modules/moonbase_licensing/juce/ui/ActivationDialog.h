#pragma once

// One-call helper to pop the activation flow in its own window. For a plugin
// editor you'll usually embed ActivationComponent directly instead; this is for
// standalone apps and "Activate…" menu items.

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../ActivationConfig.h"
#include "ActivationComponent.h"

namespace moonbase::juce_integration {

class ActivationDialog
{
public:
    // Opens a centered, self-owning window hosting an ActivationComponent. The
    // window closes when the user dismisses the flow (Open / Continue) or via
    // the title-bar close button; onClosed reports whether a valid license is
    // active at close time.
    static void show(ActivationConfig config, std::function<void(bool wasActivated)> onClosed = {});
};

} // namespace moonbase::juce_integration
