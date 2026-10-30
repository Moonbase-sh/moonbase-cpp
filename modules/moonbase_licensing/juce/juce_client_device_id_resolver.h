#pragma once

// The device id the moonbase_JUCEClient module binds a license to, so a plugin
// moving from that module to this one can recognise the devices its users have.
//
// With ActivationConfig::juceClientLicenseFile set, the default resolver accepts
// this id alongside the cross-SDK one it binds, so the old module's licenses
// convert and each device moves to the cross-SDK id (see
// ActivationConfig::resolvedDeviceIdResolver()). Name it yourself for a custom
// salt, or to keep binding the old id instead of moving:
//
//     config.deviceIdResolver =
//         std::make_shared<moonbase::juce_integration::juce_client_device_id_resolver>();
//
// Moonbase keys a seat and a trial by the device id, so binding the old id keeps
// the seat and the trial the device holds, and lets a build on either module run
// on the same computer against one seat.
//
// That module hashes, on every platform:
//
//     lowercase hex SHA-256 of the UTF-8 of
//         platform letter + salt + juce::SystemStats::getUniqueDeviceID()
//
// with the letter M (macOS), W (Windows), L (Linux), B (BSD), I (iOS) or
// A (Android), and the salt "moonsalt_1" unless the plugin set
// APIInitializer::customMachineHashSalt, which goes in the constructor here. A
// plugin that set APIInitializer::machineIdCallback bound whatever that returned,
// so it wants a resolver returning the same value instead of this one.
//
// Like legacy_juce_device_id_resolver, this rests on getUniqueDeviceID(), JUCE's
// own derivation rather than a published format. It reproduces the id only while
// the plugin ships a JUCE that derives it the same way as the one the old module
// shipped with, so do not combine the move with a JUCE upgrade. It is also not the
// cross-SDK fingerprint spec: an app built on @moonbase.sh/licensing sees a computer
// bound to it as a different device. See docs/migration-juceclient.md.

#include <string>
#include <string_view>
#include <utility>

#include <moonbase/detail/crypto/crypto.hpp>
#include <moonbase/device_id_resolver.hpp>
#include <moonbase/errors.hpp>

#include <juce_core/juce_core.h>

#include "legacy_juce_device_id_resolver.h"

namespace moonbase::juce_integration {

namespace detail {

// The letter moonbase_JUCEClient put first, as juce::OnlineUnlockStatus does.
[[nodiscard]] constexpr char juceClientPlatformLetter() noexcept
{
#if JUCE_MAC
    return 'M';
#elif JUCE_WINDOWS
    return 'W';
#elif JUCE_LINUX
    return 'L';
#elif JUCE_BSD
    return 'B';
#elif JUCE_IOS
    return 'I';
#elif JUCE_ANDROID
    return 'A';
#else
    return '\0';
#endif
}

} // namespace detail

class juce_client_device_id_resolver : public moonbase::device_id_resolver
{
public:
    // The salt the module used unless APIInitializer::customMachineHashSalt was set.
    static constexpr const char* default_salt = "moonsalt_1";

    // False on a JUCE without SystemStats::getUniqueDeviceID(); device_id()
    // throws a configuration_error there rather than returning a wrong id.
    static constexpr bool is_available = legacy_juce_device_id_resolver::is_available;

    explicit juce_client_device_id_resolver(std::string salt = default_salt) : salt_(std::move(salt)) {}

    [[nodiscard]] std::string device_name() const override
    {
        return juce::SystemStats::getComputerName().toStdString();
    }

    [[nodiscard]] std::string device_id() const override
    {
        if constexpr (detail::juceClientPlatformLetter() == '\0')
        {
            throw moonbase::configuration_error(
                "juce_client_device_id_resolver has no platform letter for this platform, so there is no "
                "moonbase_JUCEClient device id to reproduce.");
        }
        else
        {
            return hash(detail::juceClientPlatformLetter(), salt_,
                        detail::legacyJuceDeviceId("juce_client_device_id_resolver"));
        }
    }

    // The id the module bound for these inputs.
    [[nodiscard]] static std::string hash(char platformLetter, std::string_view salt, std::string_view uniqueDeviceId)
    {
        std::string material(1, platformLetter);
        material.append(salt);
        material.append(uniqueDeviceId);
        return moonbase::detail::sha256_hex(material);
    }

    // The letter the module put first on this platform; '\0' on one it didn't build for.
    [[nodiscard]] static constexpr char platform_letter() noexcept { return detail::juceClientPlatformLetter(); }

private:
    std::string salt_;
};

} // namespace moonbase::juce_integration
