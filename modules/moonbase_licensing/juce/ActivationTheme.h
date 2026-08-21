#pragma once

// Colour + typeface tokens for the built-in activation UI, from the "Solstice
// Activation" design.
//
// Set them on ActivationConfig (config.palette, config.fonts) before you build
// an ActivationComponent. The component reads the theme once, when it creates
// its LookAndFeel and pre-renders its icons, so a theme handed over afterwards
// would only reach half the screen.

#include <functional>

#include <juce_graphics/juce_graphics.h>

namespace moonbase::juce_integration {

// Every colour in the UI except the accent, which stays its own
// ActivationConfig field because it is the one-line way to brand the flow.
// Override the tokens you care about and leave the rest at the design's values:
//
//   config.palette.backgroundTop = juce::Colour(0xff1a1512);
//   config.palette.textPrimary   = juce::Colour(0xfff7f2ea);
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
    juce::Colour panelShadow      { 0x73000000 }; // soft drop shadow under the panel
    juce::Colour overlayDim       { 0x94000000 }; // scrim when config.overlayBackdrop is set

    // Surfaces inside the panel.
    juce::Colour cardFill    { 0x06ffffff }; // license/seat/notes cards, drop zone, feature field
    juce::Colour trackFill   { 0x12ffffff }; // unfilled part of a progress bar
    juce::Colour skeleton    { 0x10ffffff }; // loading placeholder bars
    juce::Colour scrollThumb { 0x80ffffff };
    juce::Colour scrollTrack { 0x1affffff };

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
    juce::Colour onAccent    { 0xffffffff }; // text + icons drawn on an accent fill
    juce::Colour seatEmpty   { 0x1affffff }; // seat pips this license has not used

    // Motion: the panel's breathing top-edge glow and the activation spinner.
    juce::Colour glow         { 0xff82cef1 };
    juce::Colour spinnerTrack { 0x26ffffff };

    // Status.
    juce::Colour success       { 0xff34d27b };
    juce::Colour successFill   { 0x2416a34a };
    juce::Colour successBorder { 0x5916a34a };
    juce::Colour trial         { 0xffeab308 };
    juce::Colour trialBright   { 0xfff5c542 }; // bright end of the trial progress gradient
    juce::Colour onTrial       { 0xff131519 }; // text drawn on top of a `trial` fill
    juce::Colour error         { 0xfff08a8a };
    juce::Colour errorStrong   { 0xffdc5050 }; // expired bar + the pill washes derived from it
    juce::Colour errorDeep     { 0xffb9444c }; // dark end of the expired progress gradient
    juce::Colour dangerFill    { 0x14dc5050 };
    juce::Colour dangerBorder  { 0x4cdc5050 };
};

// Optional typefaces for the UI's three font roles. Leave a role null and it
// falls back to the platform default (sans, sans Bold, monospaced).
//
// Bundle your own with juce_add_binary_data and hand them over here rather than
// setting a process-wide default LookAndFeel: inside a DAW that default is
// shared with the host and every other plugin loaded in the same process.
//
//   config.fonts.body = juce::Typeface::createSystemTypefaceFor(
//       BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize);
struct ActivationFonts
{
    enum class Role
    {
        heading, // titles, buttons, pills. Supply a bold face: no style is applied on top.
        body,    // paragraphs, labels, links
        mono     // device id chip, license file names
    };

    juce::Typeface::Ptr heading;
    juce::Typeface::Ptr body;
    juce::Typeface::Ptr mono;

    // Last-word hook, for when a single typeface per role is not enough (a
    // variable font, per-size style choices, your own juce::Font cache). When
    // set it decides every font the UI asks for, and the fields above are unused.
    std::function<juce::Font(Role role, float height)> makeFont;
};

} // namespace moonbase::juce_integration
