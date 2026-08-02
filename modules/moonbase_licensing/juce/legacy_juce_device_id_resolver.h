#pragma once

// The device id this module used before it adopted the cross-SDK fingerprint
// spec: juce::SystemStats::getUniqueDeviceID().
//
// FROZEN, and no longer the default. It is kept so a plugin that already has
// activated users can keep validating their licenses, by naming it as a
// historical resolver:
//
//     config.deviceIdResolver = std::make_shared<moonbase::migrating_device_id_resolver>(
//         ActivationConfig::defaultDeviceIdResolver(),
//         std::make_shared<moonbase::juce_integration::legacy_juce_device_id_resolver>());
//
// Two reasons it is not the default any more. It is not the spec, so a license
// activated in a web or Electron app built on @moonbase.sh/licensing would never
// validate in the plugin, or the other way round. And getUniqueDeviceID() is
// JUCE's own derivation rather than a published format, so it can change between
// JUCE versions: this resolver only vouches for a binding if the plugin still
// ships the JUCE version that created it.
//
// The original reason for preferring it, that the SDK used to shell out to
// ioreg/dmidecode and a sandboxed plugin host blocks that, no longer applies.
// moonbase_device_id_resolver reads IOKit, world-readable files and the firmware
// table directly, and spawns no subprocess on any platform.

#include <string>

#include <moonbase/device_id_resolver.hpp>

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class legacy_juce_device_id_resolver : public moonbase::device_id_resolver
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

#if !defined(MOONBASE_DISABLE_DEPRECATED_ALIASES)

using juce_fingerprint_provider
    [[deprecated("renamed to moonbase::juce_integration::legacy_juce_device_id_resolver; note that the "
                 "module default is now moonbase::moonbase_device_id_resolver, which implements the "
                 "cross-SDK fingerprint spec")]] = legacy_juce_device_id_resolver;

#endif

} // namespace moonbase::juce_integration
