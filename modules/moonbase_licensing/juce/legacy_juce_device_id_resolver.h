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
// ships the JUCE version that created it. It also only exists from JUCE 7.0.5 on
// (and in HISE's backported 6.1.3), so see is_available below.
//
// The original reason for preferring it, that the SDK used to shell out to
// ioreg/dmidecode and a sandboxed plugin host blocks that, no longer applies.
// moonbase_device_id_resolver reads IOKit, world-readable files and the firmware
// table directly, and spawns no subprocess on any platform.

#include <string>
#include <type_traits>

#include <moonbase/device_id_resolver.hpp>
#include <moonbase/errors.hpp>

#include <juce_core/juce_core.h>

#include "JuceCompat.h"

namespace moonbase::juce_integration {

namespace detail {

// getUniqueDeviceID() only landed upstream in JUCE 7.0.5, but HISE backported it
// into the JUCE 6.1.3 it pins. Detect the method rather than the version, so
// both get the real thing and neither gets a build failure.
template <typename Stats, typename = void>
struct has_unique_device_id : std::false_type
{
};

template <typename Stats>
struct has_unique_device_id<Stats, std::void_t<decltype(Stats::getUniqueDeviceID())>> : std::true_type
{
};

// Stats is a template parameter so the call is dependent, and the branch that
// does not compile on this JUCE is never instantiated.
template <typename Stats = juce::SystemStats>
[[nodiscard]] std::string legacyJuceDeviceId()
{
    if constexpr (has_unique_device_id<Stats>::value)
    {
        return Stats::getUniqueDeviceID().toStdString();
    }
    else
    {
        throw moonbase::configuration_error(
            "legacy_juce_device_id_resolver needs juce::SystemStats::getUniqueDeviceID(), which this "
            "JUCE does not have (it was added in JUCE 7.0.5). No license can have been bound with it "
            "by a build on this JUCE, so remove the resolver from your migration chain.");
    }
}

} // namespace detail

class legacy_juce_device_id_resolver : public moonbase::device_id_resolver
{
public:
    // False on a JUCE without SystemStats::getUniqueDeviceID(); device_id()
    // throws a configuration_error there rather than returning a wrong id.
    static constexpr bool is_available = detail::has_unique_device_id<juce::SystemStats>::value;

    [[nodiscard]] std::string device_name() const override
    {
        return juce::SystemStats::getComputerName().toStdString();
    }

    [[nodiscard]] std::string device_id() const override { return detail::legacyJuceDeviceId(); }
};

#if !defined(MOONBASE_DISABLE_DEPRECATED_ALIASES)

using juce_fingerprint_provider
    [[deprecated("renamed to moonbase::juce_integration::legacy_juce_device_id_resolver; note that the "
                 "module default is now moonbase::moonbase_device_id_resolver, which implements the "
                 "cross-SDK fingerprint spec")]] = legacy_juce_device_id_resolver;

#endif

} // namespace moonbase::juce_integration
