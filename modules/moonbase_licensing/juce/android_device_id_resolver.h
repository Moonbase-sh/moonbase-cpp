#pragma once

// Scoped device identity for Android, per the fingerprint spec's "Scoped
// identity" section.
//
// Android exposes no device identifier that unrelated applications can read:
// Build.SERIAL returns "unknown" without a privileged permission from Android 10,
// IMEI needs READ_PRIVILEGED_PHONE_STATE, MAC addresses are randomised, and
// Settings.Secure.ANDROID_ID has been scoped to the app signing key since
// Android 8. So the id is scoped and stamped `mbd2s_`.
//
// DO NOT reach for juce::SystemStats::getUniqueDeviceID() here. It reads the
// *static field* Settings.Secure.ANDROID_ID via GetStaticObjectField, which is the
// key name "android_id" rather than the device's value, and hashes that. Every
// JUCE Android app therefore reports the same id, so one activation would unlock
// the entire install base. Its jassert that the result is non-empty never fires,
// because the hash of a constant is not empty, so the defect is silent. It is
// still present in JUCE 9.0.0.
//
// This resolver calls Settings.Secure.getString(contentResolver, "android_id")
// through JNI instead, and the spec's ^[0-9a-f]{1,16}$ rule then makes the mistake
// mechanically impossible: "android_id" is not hex, so it can never reach the
// material even if this code regressed.

#include <memory>
#include <optional>
#include <string>

#include <moonbase/device_id_resolver.hpp>
#include <moonbase/fingerprint_spec.hpp>

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class android_device_id_resolver : public moonbase::device_id_resolver
{
public:
    /// Settings.Secure.getString(contentResolver, ANDROID_ID), lowercased.
    ///
    /// Empty when Android declines to provide one: the value is generated lazily
    /// and getString can return null. Absence is transient and must surface as
    /// insufficient identity rather than as a constant.
    [[nodiscard]] static std::string readAndroidId()
    {
#if JUCE_ANDROID
        auto* env = juce::getEnv();
        if (env == nullptr)
            return {};

        // Settings.Secure.getString(ContentResolver, String). The second argument
        // is the literal key, NOT the static field: reading the field yields the
        // key name itself, which is what the JUCE defect above does.
        juce::LocalRef<jclass> secure(
            (jclass) env->FindClass("android/provider/Settings$Secure"));
        if (secure.get() == nullptr)
        {
            env->ExceptionClear();
            return {};
        }

        const auto getString = env->GetStaticMethodID(
            secure.get(),
            "getString",
            "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
        if (getString == nullptr)
        {
            env->ExceptionClear();
            return {};
        }

        auto context = juce::getAppContext();
        if (context.get() == nullptr)
            return {};

        juce::LocalRef<jobject> resolver(env->CallObjectMethod(
            context.get(),
            env->GetMethodID(
                env->GetObjectClass(context.get()),
                "getContentResolver",
                "()Landroid/content/ContentResolver;")));
        if (resolver.get() == nullptr)
        {
            env->ExceptionClear();
            return {};
        }

        juce::LocalRef<jstring> key(env->NewStringUTF("android_id"));
        juce::LocalRef<jstring> value((jstring) env->CallStaticObjectMethod(
            secure.get(), getString, resolver.get(), key.get()));

        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
            return {};
        }
        if (value.get() == nullptr)
            return {};

        return juce::juceString(env, value.get()).toLowerCase().toStdString();
#else
        return {};
#endif
    }

    [[nodiscard]] std::string device_name() const override
    {
        // Decoration only: it never enters the material, because the host-name
        // fallback is forbidden on Android. Build.MODEL on most devices.
        return juce::SystemStats::getDeviceDescription().toStdString();
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

        const fp::parameter_list params{{"androidId", readAndroidId()}};
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
