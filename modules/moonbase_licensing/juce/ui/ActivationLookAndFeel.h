#pragma once

// Palette, fonts and icon helpers for the built-in activation UI, ported from
// the "Solstice Activation" design. The accent colour comes from
// ActivationConfig; everything else is a themeable token here. Subclass or
// mutate the palette to re-skin the whole flow.

#include <memory>

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace moonbase::juce_integration {

struct ActivationPalette
{
    // Backdrop + plugin window.
    juce::Colour backgroundTop    { 0xff0e1626 };
    juce::Colour backgroundMid    { 0xff070a11 };
    juce::Colour backgroundBottom { 0xff04060b };
    juce::Colour panelTop         { 0xff0d121c };
    juce::Colour panelMid         { 0xff080b13 };
    juce::Colour panelBottom      { 0xff06090f };
    juce::Colour panelBorder      { 0x14ffffff };
    juce::Colour hairline         { 0x1affffff };

    // Text.
    juce::Colour textPrimary   { 0xfff5f8fb };
    juce::Colour textBody      { 0xffcdd8e6 };
    juce::Colour textBright    { 0xff9fb3cc };
    juce::Colour textSecondary { 0xff768aa4 };
    juce::Colour textMuted     { 0xff5a6b82 };

    // Controls.
    juce::Colour ghostFill   { 0x0affffff };
    juce::Colour ghostBorder { 0x21ffffff };
    juce::Colour ghostHover  { 0x14ffffff };
    juce::Colour link        { 0xff6aa8ff };

    // Status.
    juce::Colour success       { 0xff34d27b };
    juce::Colour successFill   { 0x2416a34a };
    juce::Colour successBorder { 0x5916a34a };
    juce::Colour trial         { 0xffeab308 };
    juce::Colour error         { 0xfff08a8a };
    juce::Colour dangerFill    { 0x14dc5050 };
    juce::Colour dangerBorder  { 0x4cdc5050 };
};

class ActivationLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit ActivationLookAndFeel(juce::Colour accentColour = juce::Colour(0xff186cdc))
        : accent(accentColour)
    {
        setColour(juce::ResizableWindow::backgroundColourId, palette.backgroundBottom);
    }

    juce::Colour accent;
    ActivationPalette palette;

    // Fonts: Inter ~ default sans, Space Mono ~ default monospaced. Bundle real
    // typefaces with juce_add_binary_data and swap these if you want exact
    // fidelity.
    [[nodiscard]] juce::Font heading(float height) const
    {
        return juce::Font(juce::FontOptions().withHeight(height).withStyle("Bold"));
    }
    [[nodiscard]] juce::Font body(float height) const
    {
        return juce::Font(juce::FontOptions().withHeight(height));
    }
    [[nodiscard]] juce::Font mono(float height) const
    {
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

} // namespace icons

} // namespace moonbase::juce_integration
