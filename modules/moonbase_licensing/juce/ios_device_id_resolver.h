#pragma once

// Scoped device identity for iOS, per the fingerprint spec's "Scoped identity"
// section.
//
// iOS provides no device identifier that unrelated applications can read: the
// hardware serial has been inaccessible since iOS 7, and identifierForVendor is
// scoped to the app's vendor prefix, so two publishers see different values on
// the same device. The spec's answer is not to pretend otherwise but to stamp the
// limitation into the id, as `mbd2s_`, so the server, support and any validator
// know the value is stable within this publisher and meaningless outside it.
//
// This is deliberately NOT the spec's host-name fallback. iOS device names are
// routinely the literal "iPhone", so hashing one would hand an enormous
// population a single device id and let them validate each other's licenses,
// which is exactly the failure the insufficient-identity rule exists to prevent.
//
// identifierForVendor is nil until the device is first unlocked after boot, and
// resets if the user removes every app from this vendor. Both cases surface as
// insufficient_device_identity_error rather than as a constant.

#include <memory>
#include <optional>
#include <string>

#include <moonbase/device_id_resolver.hpp>
#include <moonbase/fingerprint_spec.hpp>

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class ios_device_id_resolver : public moonbase::device_id_resolver
{
public:
    /// The vendor identifier, uppercased with hyphens removed, matching how the
    /// spec treats ioPlatformUuid. Empty when iOS declines to provide one.
    ///
    /// Sourced through juce::SystemStats, which wraps
    /// [[UIDevice currentDevice] identifierForVendor] on iOS.
    [[nodiscard]] static std::string readIdentifierForVendor()
    {
        return moonbase::fingerprint_spec::normalize_platform_uuid(
            juce::SystemStats::getUniqueDeviceID().toStdString());
    }

    [[nodiscard]] std::string device_name() const override
    {
        return juce::SystemStats::getComputerName().toStdString();
    }

    [[nodiscard]] std::string device_id() const override { return describe().device_id; }

    [[nodiscard]] std::optional<moonbase::device_id_description> describe_device() const override
    {
        return describe();
    }

private:
    [[nodiscard]] moonbase::device_id_description describe() const
    {
        namespace fp = moonbase::fingerprint_spec;

        const fp::parameter_list params{{"identifierForVendor", readIdentifierForVendor()}};
        const auto platform = std::string(fp::platform_tag());

        moonbase::device_id_description described;
        described.device_id = fp::fingerprint_device_id(
            fp::build_fingerprint_material(platform, params), fp::device_id_source::scoped);
        described.version = fp::version;
        described.platform = platform;
        described.source = fp::device_id_source::scoped;
        for (const auto& param : fp::canonicalize_params(params))
            described.param_names.push_back(param.first);
        return described;
    }
};

} // namespace moonbase::juce_integration
