#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace moonbase {

// New values are appended, never inserted: consumers persist and compare these.
enum class error_type {
    api_error,
    license_invalid,
    license_expired,
    storage_error,
    configuration_error,
    operation_not_supported,
    /// The license is valid but bound to a different device, or to an older
    /// fingerprint version.
    license_device_mismatch,
    /// No stable hardware identifier could be read, so no device id exists.
    device_identity_unavailable,
};

class moonbase_error : public std::runtime_error {
public:
    moonbase_error(error_type type, std::string message)
        : std::runtime_error(std::move(message)), type_(type)
    {
    }

    [[nodiscard]] error_type type() const noexcept { return type_; }

private:
    error_type type_;
};

class configuration_error : public moonbase_error {
public:
    explicit configuration_error(const std::string& message)
        : moonbase_error(error_type::configuration_error, message)
    {
    }
};

class api_error : public moonbase_error {
public:
    api_error(int status_code, std::string message, std::string title = {}, std::string detail = {})
        : moonbase_error(error_type::api_error, std::move(message)),
          status_code_(status_code),
          title_(std::move(title)),
          detail_(std::move(detail))
    {
    }

    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    int status_code_;
    std::string title_;
    std::string detail_;
};

class license_invalid_error : public moonbase_error {
public:
    explicit license_invalid_error(const std::string& message)
        : moonbase_error(error_type::license_invalid, message)
    {
    }

protected:
    // For subclasses that are a more specific kind of "this license is not
    // usable here" and want their own error_type.
    license_invalid_error(error_type type, const std::string& message)
        : moonbase_error(type, message)
    {
    }
};

// The token verified, but its `sig` claim is not this device's id and no
// historical resolver recognised it.
//
// Derives from license_invalid_error deliberately. Existing `catch
// (license_invalid_error&)` sites keep working across the upgrade, and, more
// importantly, licensing's offline grace period keys off that type: a mismatch
// that escaped it would let a license copied from another machine keep running
// for the whole grace window. Code switching on type() must add the new case.
class license_device_mismatch_error : public license_invalid_error {
public:
    explicit license_device_mismatch_error(const std::string& message)
        : license_invalid_error(error_type::license_device_mismatch, message)
    {
    }
};

// The device fingerprint had nothing machine-specific to hash: either no
// parameter could be read, or the only ones that could are model-level (vendor,
// product and board names, shared by every unit of a product line).
//
// The spec makes both an error rather than hashing what is there, because either
// would hand a whole class of machines the *same* device id, and a license bound
// to it would then validate on all of them. Substituting the host name is nearly
// as bad: it is user-renameable, duplicated across imaged fleets, and
// regenerated on every container start.
//
// Reachable on platforms with no defined identity parameters (Android, BSD,
// anything unknown); when every source fails, such as a container with no DMI or
// a blocked firmware-table read; and on machines whose per-device identifiers are
// simply absent, such as a Linux install with no machine-id or a VM whose SMBIOS
// carries an unset UUID alongside a blank baseboard serial. Opt into the weaker
// host-name id with moonbase_device_id_resolver_options::fallback.
class insufficient_device_identity_error : public moonbase_error {
public:
    explicit insufficient_device_identity_error(
        std::string platform,
        std::string reason = "no identity parameter could be read")
        : moonbase_error(
              error_type::device_identity_unavailable,
              "Could not identify this device (platform: " + platform + "): " + reason),
          platform_(std::move(platform)),
          reason_(std::move(reason))
    {
    }

    [[nodiscard]] const std::string& platform() const noexcept { return platform_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

private:
    std::string platform_;
    std::string reason_;
};

// Two fingerprint parameters shared a name, which the material grammar cannot
// express. Unreachable from the built-in readers, so it always means a
// caller-supplied parameter list is wrong: a configuration error, not a
// machine-state one. No matching error_type, because no other Moonbase SDK
// reports this on its error enum.
class duplicate_fingerprint_parameter_error : public configuration_error {
public:
    explicit duplicate_fingerprint_parameter_error(std::string name)
        : configuration_error("Duplicate fingerprint parameter name: " + name),
          parameter_name_(std::move(name))
    {
    }

    [[nodiscard]] const std::string& parameter_name() const noexcept { return parameter_name_; }

private:
    std::string parameter_name_;
};

class license_expired_error : public moonbase_error {
public:
    explicit license_expired_error(const std::string& message)
        : moonbase_error(error_type::license_expired, message)
    {
    }
};

class storage_error : public moonbase_error {
public:
    explicit storage_error(const std::string& message)
        : moonbase_error(error_type::storage_error, message)
    {
    }
};

class operation_not_supported_error : public moonbase_error {
public:
    explicit operation_not_supported_error(const std::string& message)
        : moonbase_error(error_type::operation_not_supported, message)
    {
    }
};

} // namespace moonbase
