#pragma once

// Palette, fonts and icon helpers for the built-in activation UI, ported from
// the "Solstice Activation" design. The accent colour, the palette and the
// typefaces all come from ActivationConfig; this class is the resolved view of
// them that the screens paint through.

#include <memory>
#include <utility>

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../ActivationTheme.h"

namespace moonbase::juce_integration {

class ActivationLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit ActivationLookAndFeel(juce::Colour accentColour = juce::Colour(0xff186cdc),
                                   ActivationPalette paletteIn = {},
                                   ActivationFonts fontsIn = {})
        : accent(accentColour), palette(std::move(paletteIn)), fonts(std::move(fontsIn))
    {
        setColour(juce::ResizableWindow::backgroundColourId, palette.backgroundBottom);
    }

    juce::Colour accent;
    ActivationPalette palette;
    ActivationFonts fonts;

    // Fonts: Inter ~ default sans, Space Mono ~ default monospaced. Bundle real
    // typefaces with juce_add_binary_data and set config.fonts for exact
    // fidelity; each role falls back to the platform default when left unset.
    [[nodiscard]] juce::Font heading(float height) const
    {
        if (fonts.makeFont)
            return fonts.makeFont(ActivationFonts::Role::heading, height);
        if (fonts.heading != nullptr)
            return juce::Font(juce::FontOptions().withTypeface(fonts.heading).withHeight(height));
        return juce::Font(juce::FontOptions().withHeight(height).withStyle("Bold"));
    }
    [[nodiscard]] juce::Font body(float height) const
    {
        if (fonts.makeFont)
            return fonts.makeFont(ActivationFonts::Role::body, height);
        if (fonts.body != nullptr)
            return juce::Font(juce::FontOptions().withTypeface(fonts.body).withHeight(height));
        return juce::Font(juce::FontOptions().withHeight(height));
    }
    [[nodiscard]] juce::Font mono(float height) const
    {
        if (fonts.makeFont)
            return fonts.makeFont(ActivationFonts::Role::mono, height);
        if (fonts.mono != nullptr)
            return juce::Font(juce::FontOptions().withTypeface(fonts.mono).withHeight(height));
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height,
                                            juce::Font::plain));
    }
};

//==============================================================================
// Icon helper: builds a juce::Drawable from inline SVG <path> data, stroked (or
// filled) in a chosen colour. The design's icons are 24x24 stroke paths.
namespace icons {

std::unique_ptr<juce::Drawable> fromStroke(const juce::String& pathData,
                                           juce::Colour colour,
                                           float strokeWidth = 1.7f,
                                           float viewBox = 24.0f);

std::unique_ptr<juce::Drawable> fromFill(const juce::String& pathData,
                                         juce::Colour colour,
                                         float viewBox = 24.0f);

// The official Moonbase brand mark (from the marketing site's
// images/logos/moonbase.svg), recoloured to a single colour.
std::unique_ptr<juce::Drawable> moonbaseMark(juce::Colour colour);

// Path data strings (24x24 viewBox unless noted) used across the screens.
extern const char* const offlineGlobe;
extern const char* const back;
extern const char* const upload;
extern const char* const fileDown;
extern const char* const checkCircle;
extern const char* const fileQuestion;
extern const char* const warning;
extern const char* const lock;
extern const char* const externalLink;
extern const char* const monitor;
extern const char* const check;
extern const char* const cross;
extern const char* const downloadTray; // arrow into a tray (update / download)

} // namespace icons

} // namespace moonbase::juce_integration
