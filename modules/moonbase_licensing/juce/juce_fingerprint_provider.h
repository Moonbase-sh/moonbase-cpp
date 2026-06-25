#pragma once

// Device fingerprint sourced from juce::SystemStats::getUniqueDeviceID(), which
// JUCE derives from stable hardware identifiers (JUCE 7+). Used by default so
// the module never shells out to ioreg/dmidecode from inside a sandboxed plugin
// host. Pick one provider when you ship and keep it — changing it changes the
// device id Moonbase sees and invalidates existing activations.

#include <string>

#include <moonbase/moonbase.hpp>

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class juce_fingerprint_provider : public moonbase::fingerprint_provider
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

} // namespace moonbase::juce_integration
