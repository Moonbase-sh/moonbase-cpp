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

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
// identifierForVendor lives in UIKit, but reaching it needs no Objective-C source
// and no framework wrapper: the Objective-C runtime's C API is callable straight
// from C++, which keeps this SDK usable without JUCE or any other framework.
#define MOONBASE_FINGERPRINT_USE_UIKIT 1
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#endif

#if defined(__ANDROID__)
// Plain JNI, part of the NDK rather than any framework. The one thing a native
// library cannot obtain by itself is the application Context, so the host hands
// that in once; see moonbase::android::set_jni_environment below.
#include <jni.h>
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

#if defined(__ANDROID__)
namespace android {

/// The JNI handles the Android device id reader needs.
///
/// Everything else in this SDK reads the machine on its own. Android is the one
/// exception, and not for want of trying: Settings.Secure.getString needs a
/// ContentResolver, which needs an application Context, and there is no supported
/// way for a native library to obtain one by itself. So the host hands it in once
/// and the SDK does the rest, rather than this SDK depending on a framework.
struct jni_handles {
    JavaVM* vm = nullptr;
    jobject context = nullptr;
};

namespace detail {

inline jni_handles& mutable_jni_environment()
{
    static jni_handles handles;
    return handles;
}

[[nodiscard]] inline jni_handles jni_environment()
{
    return mutable_jni_environment();
}

} // namespace detail

/// Supply the JNI handles, once, during startup. Typically from JNI_OnLoad:
///
///     jint JNI_OnLoad(JavaVM* vm, void*) {
///         moonbase::android::set_jni_environment(vm, applicationContext);
///         return JNI_VERSION_1_6;
///     }
///
/// The JUCE module does this for you. Until it is called, Android resolves to
/// insufficient_device_identity_error rather than to a constant, which is the
/// honest answer for a machine the SDK cannot identify.
///
/// `context` must outlive the SDK, so pass a global reference or the Application
/// object rather than an Activity.
inline void set_jni_environment(JavaVM* vm, jobject context)
{
    detail::mutable_jni_environment() = jni_handles{vm, context};
}

} // namespace android
#endif


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
        identity.params.emplace_back("androidId", read_android_id());
#elif defined(MOONBASE_FINGERPRINT_USE_UIKIT)
        identity.params.emplace_back("identifierForVendor", read_identifier_for_vendor());
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
    // Both memos are a mutex plus a flag rather than std::once_flag, and that is
    // deliberate: description() lets insufficient_device_identity_error escape so
    // an unreadable machine retries instead of caching the failure, and
    // std::call_once is a bad place to throw from. libstdc++ implements it on
    // pthread_once, and ThreadSanitizer's pthread_once interceptor does not model
    // the reset that the exception path performs, so the *second* call deadlocks.
    // The plain mutex gives the same semantics with none of that: the flag is only
    // set after the value is stored, so a throw leaves it false and the next
    // caller tries again.
    //
    // Returning a reference is safe because neither value is ever mutated once its
    // flag is set, and the lock establishes the happens-before edge for the reader.

    [[nodiscard]] const device_identity& identity() const
    {
        // Read at most once. Both halves of an activation request ask for it, the
        // name and then the id, and the validator asks for the id on every single
        // token check. Reading per call would also let the name and the id come
        // from two different reads of the machine.
        const std::lock_guard<std::mutex> lock(identity_mutex_);
        if (!identity_read_) {
            try {
                identity_ = options_.reader ? options_.reader() : read_host_identity();
            } catch (...) {
                // Reads are best-effort. An unreadable machine is insufficient
                // identity, which device_id() reports, not an exception thrown
                // out of device_name().
                identity_ = device_identity{};
            }
            identity_read_ = true;
        }
        return identity_;
    }

    [[nodiscard]] const device_id_description& description() const
    {
        // A machine that is momentarily unreadable retries on the next call rather
        // than caching the failure: describe() throws, described_ stays false, and
        // the guard releases the lock on the way out. A sticky failure would
        // outlive the condition that caused it.
        const std::lock_guard<std::mutex> lock(description_mutex_);
        if (!described_) {
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
            described_ = true;
        }
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

#if defined(MOONBASE_FINGERPRINT_USE_UIKIT)
    /// identifierForVendor, uppercased with hyphens removed like ioPlatformUuid.
    ///
    /// Reached through the Objective-C runtime's C API rather than Objective-C
    /// source, so this header stays plain C++ and this SDK needs no framework to
    /// fingerprint an iOS device. Empty when iOS declines to provide one, which it
    /// does until the device is first unlocked after boot; absence is transient and
    /// surfaces as insufficient identity rather than as a constant.
    [[nodiscard]] static std::string read_identifier_for_vendor()
    {
        using send_id = id (*)(id, SEL);
        using send_cstr = const char* (*)(id, SEL);
        const auto msg_id = reinterpret_cast<send_id>(objc_msgSend);
        const auto msg_cstr = reinterpret_cast<send_cstr>(objc_msgSend);

        // UIDevice everywhere except watchOS, which exposes the same property on
        // WKInterfaceDevice.
        Class device_class = objc_getClass("UIDevice");
        if (device_class == nullptr) {
            device_class = objc_getClass("WKInterfaceDevice");
        }
        if (device_class == nullptr) {
            return {};
        }

        id device = msg_id(reinterpret_cast<id>(device_class), sel_registerName("currentDevice"));
        if (device == nullptr) {
            return {};
        }

        id uuid = msg_id(device, sel_registerName("identifierForVendor"));
        if (uuid == nullptr) {
            return {};
        }

        id text = msg_id(uuid, sel_registerName("UUIDString"));
        if (text == nullptr) {
            return {};
        }

        const char* utf8 = msg_cstr(text, sel_registerName("UTF8String"));
        if (utf8 == nullptr) {
            return {};
        }

        return fingerprint_spec::normalize_platform_uuid(utf8);
    }
#endif

#if defined(__ANDROID__)
    /// Settings.Secure.getString(contentResolver, ANDROID_ID), lowercased.
    ///
    /// Plain JNI, so this needs no framework either. Deliberately not the static
    /// field Settings.Secure.ANDROID_ID: that is the key name "android_id",
    /// identical on every device, and hashing it would give a whole install base
    /// one device id. JUCE's SystemStats::getUniqueDeviceID() has exactly that
    /// defect. The spec's ^[0-9a-f]{1,16}$ rule makes the mistake mechanically
    /// impossible here regardless.
    ///
    /// Empty until the host supplies JNI handles (see
    /// moonbase::android::set_jni_environment), and empty when Android returns
    /// null, which it can before the user is set up.
    [[nodiscard]] static std::string read_android_id()
    {
        const auto jni = android::detail::jni_environment();
        if (jni.vm == nullptr || jni.context == nullptr) {
            return {};
        }

        JNIEnv* env = nullptr;
        if (jni.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK
            || env == nullptr) {
            return {};
        }

        const auto fail = [env] {
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            return std::string{};
        };

        jclass secure = env->FindClass("android/provider/Settings$Secure");
        if (secure == nullptr) {
            return fail();
        }

        const auto get_string = env->GetStaticMethodID(
            secure,
            "getString",
            "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
        if (get_string == nullptr) {
            return fail();
        }

        const auto get_resolver = env->GetMethodID(
            env->GetObjectClass(jni.context), "getContentResolver", "()Landroid/content/ContentResolver;");
        if (get_resolver == nullptr) {
            return fail();
        }

        jobject resolver = env->CallObjectMethod(jni.context, get_resolver);
        if (resolver == nullptr) {
            return fail();
        }

        jstring key = env->NewStringUTF("android_id");
        auto value = static_cast<jstring>(
            env->CallStaticObjectMethod(secure, get_string, resolver, key));
        if (env->ExceptionCheck() || value == nullptr) {
            return fail();
        }

        const char* utf8 = env->GetStringUTFChars(value, nullptr);
        std::string out = utf8 != nullptr ? utf8 : "";
        if (utf8 != nullptr) {
            env->ReleaseStringUTFChars(value, utf8);
        }

        // Lowercased per the spec; the ^[0-9a-f]{1,16}$ rule is case-sensitive.
        for (auto& character : out) {
            character = (character >= 'A' && character <= 'Z')
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        }
        return out;
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

    mutable std::mutex identity_mutex_;
    mutable bool identity_read_ = false;
    mutable device_identity identity_;
    mutable std::mutex description_mutex_;
    mutable bool described_ = false;
    mutable device_id_description description_;
};

} // namespace moonbase
