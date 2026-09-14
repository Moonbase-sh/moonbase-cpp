#pragma once

// JUCE version compatibility shims. The module supports JUCE 6.1 and up, which
// means a handful of APIs the UI leans on only exist over part of that range:
//
//   juce::FontOptions                        JUCE 8.0.0+
//   juce::GlyphArrangement::getStringWidth   JUCE 8.0.2+
//   juce::exactlyEqual                       JUCE 7.0.9+
//   wrapperType_LV2                          JUCE 7+     (guarded in JuceMetadata.h)
//   SystemStats::getUniqueDeviceID           JUCE 7.0.5+ (detected in
//                                            legacy_juce_device_id_resolver.h, since
//                                            HISE backported it into its JUCE 6.1.3)
//   juce_animation + AnimatorUpdater::update JUCE 8.0.4+ (see ui/ValueAnimation.h)
//
// Everything here is a thin inline wrapper picking the right spelling, so the
// rest of the module reads the same on every supported JUCE.

#include <utility>

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

// 6.1.3 is the first release with juce::URL::DownloadTaskOptions (the update
// downloader) and juce::Font::getTypefacePtr. It is also the version HISE pins.
#if JUCE_VERSION < 0x060103
  #error "moonbase_licensing needs JUCE 6.1.3 or later: earlier releases lack juce::URL::DownloadTaskOptions and juce::Font::getTypefacePtr."
#endif

// JUCE_VERSION is (major << 16) + (minor << 8) + build, so compare the whole
// thing: several of the APIs here appeared mid-8.0.x, not at 8.0.0.
#define MOONBASE_JUCE_AT_LEAST(major, minor, build) \
    (JUCE_VERSION >= (((major) << 16) + ((minor) << 8) + (build)))

// juce_animation is an optional dependency, not a declared one, so the module
// also builds on JUCE 6 and 7 where that module does not exist. Gate on JUCE's
// own "is this module linked" macro rather than __has_include, for the reason
// spelled out in JuceMetadata.h. A consumer can force it either way.
//
// The version test is not redundant: juce_animation shipped in 8.0.0, but
// AnimatorUpdater::update (double) only arrived in 8.0.4, and the module needs
// that overload to drive animations off its own timer. On 8.0.0 to 8.0.3 the
// module's own implementation is used instead.
#if !defined(MOONBASE_JUCE_HAS_ANIMATION)
  #if defined(JUCE_MODULE_AVAILABLE_juce_animation) && JUCE_MODULE_AVAILABLE_juce_animation \
      && MOONBASE_JUCE_AT_LEAST(8, 0, 4)
    #define MOONBASE_JUCE_HAS_ANIMATION 1
  #else
    #define MOONBASE_JUCE_HAS_ANIMATION 0
  #endif
#endif

namespace moonbase::juce_integration::compat {

// Fonts. JUCE 8 routes everything through juce::FontOptions; 6 and 7 use the old
// constructors, which are still present (but deprecated) on 8.
[[nodiscard]] inline juce::Font font(juce::Typeface::Ptr typeface, float height)
{
#if MOONBASE_JUCE_AT_LEAST(8, 0, 0)
    return juce::Font(juce::FontOptions().withTypeface(std::move(typeface)).withHeight(height));
#else
    return juce::Font(typeface).withHeight(height);
#endif
}

[[nodiscard]] inline juce::Font font(float height, bool bold = false)
{
#if MOONBASE_JUCE_AT_LEAST(8, 0, 0)
    const auto options = juce::FontOptions().withHeight(height);
    return juce::Font(bold ? options.withStyle("Bold") : options);
#else
    return juce::Font(height, bold ? juce::Font::bold : juce::Font::plain);
#endif
}

[[nodiscard]] inline juce::Font font(const juce::String& typefaceName, float height, bool bold = false)
{
    const int style = bold ? juce::Font::bold : juce::Font::plain;
#if MOONBASE_JUCE_AT_LEAST(8, 0, 0)
    return juce::Font(juce::FontOptions(typefaceName, height, style));
#else
    return juce::Font(typefaceName, height, style);
#endif
}

// Text measurement. Font::getStringWidthFloat is deprecated on JUCE 8 in favour
// of the static GlyphArrangement::getStringWidth, which only landed in 8.0.2.
[[nodiscard]] inline float stringWidth(const juce::Font& f, const juce::String& text)
{
#if MOONBASE_JUCE_AT_LEAST(8, 0, 2)
    return juce::GlyphArrangement::getStringWidth(f, text);
#else
    return f.getStringWidthFloat(text);
#endif
}

// juce::exactlyEqual landed in 7.0.9. Use the plain comparison everywhere below
// JUCE 8 so there is no version boundary inside the 7.0.x range; the pragmas say
// "yes, an exact float compare is what was meant here".
#if !MOONBASE_JUCE_AT_LEAST(8, 0, 0)
  #if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wfloat-equal"
  #elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wfloat-equal"
  #endif
#endif

[[nodiscard]] inline bool exactlyEqual(float a, float b) noexcept
{
#if MOONBASE_JUCE_AT_LEAST(8, 0, 0)
    return juce::exactlyEqual(a, b);
#else
    return a == b;
#endif
}

#if !MOONBASE_JUCE_AT_LEAST(8, 0, 0)
  #if defined(__clang__)
    #pragma clang diagnostic pop
  #elif defined(__GNUC__)
    #pragma GCC diagnostic pop
  #endif
#endif

} // namespace moonbase::juce_integration::compat
