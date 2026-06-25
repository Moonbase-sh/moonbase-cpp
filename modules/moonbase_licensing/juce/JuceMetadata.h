#pragma once

// Optional analytics / telemetry capture for the native module, mirroring the
// reference bridge's applyJuceMetadata(). When enabled (config.analytics.enabled),
// the controller fills moonbase::licensing_options::metadata from juce::SystemStats
// (OS, CPU, JUCE version, memory), the DAW host + plugin format (when
// juce_audio_processors is in the build), and optionally locale + app version.
// The map is sent with activation and validation requests.

#include <map>
#include <string>

#include <moonbase/moonbase.hpp>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

// Host/plugin metadata needs juce_audio_processors. Gate on JUCE's own
// "is this module linked" macro, not __has_include: JUCE puts every fetched
// module's headers on the include path, so the header is present even when the
// module isn't linked, and referencing juce::PluginHostType would then fail to
// link. A consumer can still force it with MOONBASE_JUCE_HAS_AUDIO_PROCESSORS=1.
#if !defined(MOONBASE_JUCE_HAS_AUDIO_PROCESSORS)
  #if defined(JUCE_MODULE_AVAILABLE_juce_audio_processors) && JUCE_MODULE_AVAILABLE_juce_audio_processors
    #define MOONBASE_JUCE_HAS_AUDIO_PROCESSORS 1
  #else
    #define MOONBASE_JUCE_HAS_AUDIO_PROCESSORS 0
  #endif
#endif

#if MOONBASE_JUCE_HAS_AUDIO_PROCESSORS
  #include <juce_audio_processors/juce_audio_processors.h>
#endif

namespace moonbase::juce_integration {

// What to capture. Toggle the whole thing with `enabled`; the rest narrows it.
struct AnalyticsOptions
{
    bool enabled = false;            // master switch (opt in)
    bool includeSystemInfo = true;   // OS, CPU, JUCE version, memory
    bool includeHostInfo = true;     // DAW host + plugin format (needs juce_audio_processors)
    bool includeLocaleInfo = false;  // language / region — opt in
    bool includeAppVersion = true;   // also fills application_version when unset
};

namespace detail {

inline void putIfAbsent(std::map<std::string, std::string>& map,
                        std::string key,
                        std::string value)
{
    if (value.empty())
        return;
    map.emplace(std::move(key), std::move(value));
}

#if MOONBASE_JUCE_HAS_AUDIO_PROCESSORS
inline std::string pluginFormatTag(juce::AudioProcessor::WrapperType wrapper)
{
    switch (wrapper)
    {
        case juce::AudioProcessor::wrapperType_VST:         return "VST";
        case juce::AudioProcessor::wrapperType_VST3:        return "VST3";
        case juce::AudioProcessor::wrapperType_AudioUnit:   return "AU";
        case juce::AudioProcessor::wrapperType_AudioUnitv3: return "AUv3";
        case juce::AudioProcessor::wrapperType_AAX:         return "AAX";
        case juce::AudioProcessor::wrapperType_Standalone:  return "Standalone";
        case juce::AudioProcessor::wrapperType_LV2:         return "LV2";
        case juce::AudioProcessor::wrapperType_Unity:       return "Unity";
        case juce::AudioProcessor::wrapperType_Undefined:
        default:                                            return "Unknown";
    }
}
#endif

} // namespace detail

// Fills options.metadata (and, optionally, application_version) from JUCE. Keys
// are namespaced "juce.*"; existing keys are never overwritten, so anything you
// set explicitly in config.metadata wins.
inline void applyJuceMetadata(moonbase::licensing_options& options, const AnalyticsOptions& opts)
{
    auto& meta = options.metadata;

    if (opts.includeSystemInfo)
    {
        detail::putIfAbsent(meta, "juce.version", juce::SystemStats::getJUCEVersion().toStdString());
        detail::putIfAbsent(meta, "juce.os", juce::SystemStats::getOperatingSystemName().toStdString());
        meta.emplace("juce.os.is64Bit", juce::SystemStats::isOperatingSystem64Bit() ? "true" : "false");
        detail::putIfAbsent(meta, "juce.cpu.model", juce::SystemStats::getCpuModel().trim().toStdString());
        detail::putIfAbsent(meta, "juce.cpu.vendor", juce::SystemStats::getCpuVendor().trim().toStdString());
        meta.emplace("juce.cpu.cores", std::to_string(juce::SystemStats::getNumPhysicalCpus()));
        meta.emplace("juce.cpu.threads", std::to_string(juce::SystemStats::getNumCpus()));
        meta.emplace("juce.memoryMb", std::to_string(juce::SystemStats::getMemorySizeInMegabytes()));
    }

#if MOONBASE_JUCE_HAS_AUDIO_PROCESSORS
    if (opts.includeHostInfo)
    {
        const juce::PluginHostType host;
        detail::putIfAbsent(meta, "juce.host.description", juce::String(host.getHostDescription()).toStdString());
        meta.emplace("juce.host.format", detail::pluginFormatTag(juce::PluginHostType::getPluginLoadedAs()));
    }
#else
    (void) opts.includeHostInfo;
#endif

    if (opts.includeLocaleInfo)
    {
        detail::putIfAbsent(meta, "juce.locale.display", juce::SystemStats::getDisplayLanguage().toStdString());
        detail::putIfAbsent(meta, "juce.locale.user", juce::SystemStats::getUserLanguage().toStdString());
        detail::putIfAbsent(meta, "juce.locale.region", juce::SystemStats::getUserRegion().toStdString());
    }

    if (auto* app = juce::JUCEApplicationBase::getInstance())
    {
        detail::putIfAbsent(meta, "juce.app.name", app->getApplicationName().toStdString());

        if (opts.includeAppVersion && ! options.application_version)
        {
            const auto version = app->getApplicationVersion().toStdString();
            if (! version.empty())
                options.application_version = version;
        }
    }
}

} // namespace moonbase::juce_integration
