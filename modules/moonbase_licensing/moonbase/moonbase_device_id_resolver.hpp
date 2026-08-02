#pragma once

// The default device id resolver: the Moonbase device fingerprint spec, v2.
//
// Builds the `moonbase:fingerprint:v2` material from native hardware
// identifiers (IOPlatformUUID via IOKit on macOS, machine-id plus world-readable
// DMI on Linux, SMBIOS on Windows) and stamps its SHA-256 as `mbd2_<hex>`. Every
// Moonbase SDK that implements the spec produces the same id on a given machine,
// so a license activated by one validates in the others.
//
// No subprocess is spawned on any platform, and no root-only file is read. That
// matters for plugins: IOKit works inside the App Sandbox and under a hardened
// runtime, where spawning `ioreg` does not, and it keeps the device id
// independent of whether the process happens to run elevated.
//
// The algorithm itself lives in fingerprint_spec.hpp, which has no OS headers
// and is tested on every platform. This header is only the reads.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__) && !defined(MOONBASE_FINGERPRINT_NO_IOKIT)
#include <TargetConditionals.h>
// Mac Catalyst included: it runs on macOS, can read IOKit, and takes the `mac`
// platform tag, so it must use hardware identity to agree with an Electron or web
// SDK on the same machine. TARGET_OS_IPHONE is 1 for Catalyst, so it cannot be the
// test; see platform_tag() in fingerprint_spec.hpp for the full rule.
#if ((defined(TARGET_OS_OSX) && TARGET_OS_OSX) \
     || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)) \
    && !defined(MOONBASE_FINGERPRINT_NO_IOKIT)
#define MOONBASE_FINGERPRINT_USE_IOKIT 1
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitKeys.h>
#endif
#endif

#include "moonbase/device_id_resolver.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint_spec.hpp"

namespace moonbase {

/// What a single read of the machine produced.
struct device_identity {
    fingerprint_spec::parameter_list params;
    std::string device_name;
};

using device_identity_reader = std::function<device_identity()>;

/// What to do when no hardware identity is readable.
enum class device_id_fallback {
    /// Throw insufficient_device_identity_error. The default.
    none,
    /// Hash the host name instead, producing a deliberately weaker id stamped
    /// `mbd2n_`. Opt-in, because a host name is user-renameable, frequently
    /// duplicated across imaged machines, and regenerated on every container start.
    ///
    /// **Ignored on iOS and Android**, which throw regardless: there the host name
    /// is identical on every device (since iOS 17 gethostname() returns
    /// "localhost", and UIDevice.name the model name), so the fallback would give a
    /// whole install base one id rather than merely a weak one. The refusal lives
    /// in build_fingerprint_material, so it binds a custom reader and a native
    /// bridge assembling material directly, not just this resolver.
    device_name,
};

struct moonbase_device_id_resolver_options {
    device_id_fallback fallback = device_id_fallback::none;
    /// Overrides the identity source. Primarily for testing.
    device_identity_reader reader;
    /// Overrides the detected platform tag. Primarily for testing.
    std::string platform;
};

class moonbase_device_id_resolver : public device_id_resolver {
public:
    explicit moonbase_device_id_resolver(moonbase_device_id_resolver_options options = {})
        : options_(std::move(options))
    {
        if (options_.platform.empty()) {
            options_.platform = std::string(fingerprint_spec::platform_tag());
        }
    }

    /// The host name, with a trailing ".local" removed on macOS.
    ///
    /// Never throws: a machine with no readable identity still has to be able to
    /// label itself, since activation sends the name alongside the id.
    [[nodiscard]] std::string device_name() const override { return identity().device_name; }

    /// \throws insufficient_device_identity_error when nothing identifies this
    ///         machine and the host-name fallback is not enabled.
    [[nodiscard]] std::string device_id() const override { return description().device_id; }

    /// \throws insufficient_device_identity_error, as device_id() does.
    [[nodiscard]] std::optional<device_id_description> describe_device() const override
    {
        // By value, so a caller that edits a diagnostic (or logs it through
        // something that normalizes in place) cannot change the id every later
        // call returns.
        return description();
    }

    // ------------------------------------------------------------------
    // Host reads, exposed so a consumer can inspect what this machine offers
    // without going through the resolver's memoization.

    [[nodiscard]] static device_identity read_host_identity()
    {
        device_identity identity;
        identity.device_name = read_host_name();

#if defined(MOONBASE_FINGERPRINT_USE_IOKIT)
        identity.params.emplace_back("ioPlatformUuid", read_io_platform_uuid());
#elif defined(_WIN32)
        identity.params = fingerprint_spec::parse_smbios_params(read_windows_smbios_table());
#elif defined(__ANDROID__)
        // No identity parameters are defined for Android.
#elif defined(__linux__)
        // All five sources are world-readable files, so the result does not
        // depend on privilege, on any installed CLI, or on the locale.
        const auto etc_machine_id = read_file("/etc/machine-id");
        const auto dbus_machine_id = read_file("/var/lib/dbus/machine-id");

        identity.params.emplace_back(
            "machineId", fingerprint_spec::select_machine_id({etc_machine_id, dbus_machine_id}));
        identity.params.emplace_back("sysVendor", read_file("/sys/class/dmi/id/sys_vendor"));
        identity.params.emplace_back("productName", read_file("/sys/class/dmi/id/product_name"));
        identity.params.emplace_back("boardVendor", read_file("/sys/class/dmi/id/board_vendor"));
        identity.params.emplace_back("boardName", read_file("/sys/class/dmi/id/board_name"));
#endif

        return identity;
    }

    [[nodiscard]] static std::string read_host_name()
    {
#if defined(_WIN32)
        std::array<char, 256> buffer{};
        auto size = static_cast<DWORD>(buffer.size()) - 1;
        if (GetComputerNameExA(ComputerNamePhysicalDnsHostname, buffer.data(), &size)) {
            return std::string(buffer.data(), size);
        }
        return {};
#else
        std::array<char, 256> buffer{};
        if (gethostname(buffer.data(), buffer.size() - 1) != 0) {
            return {};
        }
        std::string name(buffer.data());

#if defined(__APPLE__)
        constexpr std::string_view suffix = ".local";
        if (name.size() >= suffix.size()) {
            auto tail = name.substr(name.size() - suffix.size());
            std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (tail == suffix) {
                name.erase(name.size() - suffix.size());
            }
        }
#endif
        return name;
#endif
    }

private:
    [[nodiscard]] const device_identity& identity() const
    {
        // Read at most once. Both halves of an activation request ask for it, the
        // name and then the id, and the validator asks for the id on every single
        // token check. Reading per call would also let the name and the id come
        // from two different reads of the machine.
        std::call_once(identity_once_, [this] {
            try {
                identity_ = options_.reader ? options_.reader() : read_host_identity();
            } catch (...) {
                // Reads are best-effort. An unreadable machine is insufficient
                // identity, which device_id() reports, not an exception thrown
                // out of device_name().
                identity_ = device_identity{};
            }
        });
        return identity_;
    }

    [[nodiscard]] const device_id_description& description() const
    {
        // call_once treats an exceptional return as "not done", so a machine that
        // is momentarily unreadable retries on the next call instead of caching
        // the failure. That is deliberate: a sticky failure would outlive the
        // condition that caused it.
        std::call_once(description_once_, [this] {
            const auto& read = identity();
            try {
                description_ = describe(read.params, fingerprint_spec::device_id_source::identity);
            } catch (const insufficient_device_identity_error&) {
                if (options_.fallback != device_id_fallback::device_name) {
                    throw;
                }
                description_ = describe(
                    {{"deviceName", read.device_name}}, fingerprint_spec::device_id_source::device_name);
            }
        });
        return description_;
    }

    [[nodiscard]] device_id_description describe(
        const fingerprint_spec::parameter_list& params,
        fingerprint_spec::device_id_source source) const
    {
        const auto material = fingerprint_spec::build_fingerprint_material(options_.platform, params);

        device_id_description described;
        described.device_id = fingerprint_spec::fingerprint_device_id(material, source);
        described.version = fingerprint_spec::version;
        described.platform = options_.platform;
        described.source = source;
        for (const auto& param : fingerprint_spec::canonicalize_params(params)) {
            described.param_names.push_back(param.first);
        }
        return described;
    }

    [[nodiscard]] static std::string read_file(const char* path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return {};
        }
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }

#if defined(MOONBASE_FINGERPRINT_USE_IOKIT)
    [[nodiscard]] static std::string read_io_platform_uuid()
    {
        // MACH_PORT_NULL rather than kIOMainPortDefault or kIOMasterPortDefault:
        // both constants are defined as MACH_PORT_NULL, but the first only exists
        // in the macOS 12+ SDK and the second is deprecated there, so naming
        // either breaks somebody's -Werror build. Passing MACH_PORT_NULL selects
        // the default port and compiles against every SDK.
        const io_service_t service =
            IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOPlatformExpertDevice"));
        if (service == IO_OBJECT_NULL) {
            return {};
        }

        const CFTypeRef property = IORegistryEntryCreateCFProperty(
            service, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0);
        IOObjectRelease(service);

        if (property == nullptr) {
            return {};
        }

        std::string uuid;
        if (CFGetTypeID(property) == CFStringGetTypeID()) {
            std::array<char, 128> buffer{};
            if (CFStringGetCString(
                    static_cast<CFStringRef>(property),
                    buffer.data(),
                    static_cast<CFIndex>(buffer.size()),
                    kCFStringEncodingUTF8)) {
                uuid = buffer.data();
            }
        }
        CFRelease(property);

        // Spec: all hyphens removed and uppercased.
        return fingerprint_spec::normalize_platform_uuid(uuid);
    }
#endif

#if defined(_WIN32)
    [[nodiscard]] static std::vector<unsigned char> read_windows_smbios_table()
    {
        // 'RSMB', the raw SMBIOS firmware table provider.
        //
        // Spelled out rather than written as the multi-character literal 'RSMB',
        // which is implementation-defined and warns under -Wmultichar. Note the
        // byte order: MSDN documents "this identifier is little endian, you must
        // reverse the characters" for the *FirmwareTableID* parameter, not for
        // the provider signature, and its own sample passes 'RSMB' unreversed.
        // Reversing it here yields 0x424D5352, which no provider matches, so the
        // call returns 0 and the whole SMBIOS path silently disappears.
        constexpr DWORD rsmb = 0x52534D42;

        const DWORD size = GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
        if (size == 0) {
            return {};
        }

        std::vector<unsigned char> raw(size);
        const DWORD written = GetSystemFirmwareTable(rsmb, 0, raw.data(), size);
        if (written == 0 || written > size) {
            return {};
        }
        raw.resize(written);

        // Skip the RawSMBIOSData header (Used20CallingMethod, three version
        // bytes, then a DWORD Length); parsing starts at the first structure.
        // WMI's SMBiosData already excludes this header.
        constexpr std::size_t header_size = 8;
        if (raw.size() <= header_size) {
            return {};
        }

        std::uint32_t declared_length = 0;
        std::memcpy(&declared_length, raw.data() + 4, sizeof(declared_length));

        const std::size_t available = raw.size() - header_size;
        const auto table_size = std::min(static_cast<std::size_t>(declared_length), available);

        return std::vector<unsigned char>(
            raw.begin() + static_cast<std::ptrdiff_t>(header_size),
            raw.begin() + static_cast<std::ptrdiff_t>(header_size + table_size));
    }
#endif

    moonbase_device_id_resolver_options options_;

    mutable std::once_flag identity_once_;
    mutable device_identity identity_;
    mutable std::once_flag description_once_;
    mutable device_id_description description_;
};

} // namespace moonbase
