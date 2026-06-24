#include "ActivationComponent.h"
#include "ActivationLookAndFeel.h"

#include <juce_animation/juce_animation.h>

#include <cmath>
#include <optional>

namespace moonbase::juce_integration {

using juce::Colour;
using juce::Graphics;
using juce::Justification;
using juce::Rectangle;

//==============================================================================
// Shared drawing helpers
namespace {

constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

// juce::String(const char*) is ASCII-only, so non-ASCII glyphs (…, ·, —) must be
// decoded explicitly as UTF-8 or they mojibake.
inline juce::String u8(const char* utf8) { return juce::String::fromUTF8(utf8); }
const juce::String kMidDot = juce::String::fromUTF8(" \xc2\xb7 "); // " middot "

void drawSunLogo(Graphics& g, Rectangle<float> area, Colour accent)
{
    const auto c = area.getCentre();
    const float s = juce::jmin(area.getWidth(), area.getHeight());
    const float ring = s * 0.34f;
    const float core = s * 0.12f;
    const float lineW = juce::jmax(1.4f, s * 0.055f);

    g.setColour(accent.withAlpha(0.9f));
    g.drawEllipse(c.x - ring, c.y - ring, ring * 2.0f, ring * 2.0f, lineW);
    g.setColour(accent);
    g.fillEllipse(c.x - core, c.y - core, core * 2.0f, core * 2.0f);

    juce::Path rays;
    const float inner = s * 0.40f;
    const float outer = s * 0.49f;
    for (int i = 0; i < 8; ++i)
    {
        const float a = kTwoPi * (float) i / 8.0f;
        rays.startNewSubPath(c.x + std::cos(a) * inner, c.y + std::sin(a) * inner);
        rays.lineTo(c.x + std::cos(a) * outer, c.y + std::sin(a) * outer);
    }
    g.strokePath(rays, juce::PathStrokeType(lineW, juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
}

// Draws the product/manufacturer lockup. Uses the merchant-supplied logo when given,
// otherwise a generated sun mark. Returns the row height.
int drawBrand(Graphics& g, const ActivationLookAndFeel& lnf, const juce::Drawable* customLogo,
              Rectangle<int> row, const juce::String& product, const juce::String& manufacturer,
              float logoPx, float titlePx, bool muted = false)
{
    auto area = row;
    auto logoArea = area.removeFromLeft((int) logoPx).toFloat().withSizeKeepingCentre(logoPx, logoPx);
    if (customLogo != nullptr)
        customLogo->drawWithin(g, logoArea, juce::RectanglePlacement::centred, muted ? 0.55f : 1.0f);
    else
        drawSunLogo(g, logoArea, muted ? lnf.palette.textSecondary : lnf.accent);

    area.removeFromLeft(13);
    g.setColour(lnf.palette.textPrimary);
    g.setFont(lnf.heading(titlePx));
    const int titleH = (int) titlePx + 4;
    g.drawText(product, area.removeFromTop(titleH), Justification::bottomLeft);
    if (manufacturer.isNotEmpty())
    {
        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(11.5f));
        g.drawText(manufacturer, area.removeFromTop(16), Justification::topLeft);
    }
    return row.getHeight();
}

} // namespace

//==============================================================================
// Buttons
class StyledButton : public juce::Button,
                     private juce::Timer
{
public:
    enum class Style { accent, ghost, danger };

    StyledButton(ActivationLookAndFeel& l, Style s, const juce::String& buttonText,
                 std::unique_ptr<juce::Drawable> ic = {})
        : juce::Button(buttonText), lnf(l), style(s), icon(std::move(ic))
    {
    }

    ~StyledButton() override { stopTimer(); }

    // Cross-fade the label out and a small inline spinner in (or back out) while
    // an async action runs. Clicks are gated while busy so the work can't refire.
    void setBusy(bool shouldBeBusy)
    {
        if (busy == shouldBeBusy)
            return;
        busy = shouldBeBusy;
        setEnabled(! busy);
        startTimerHz(60);
    }

    // Reduce-motion / snapshot variant: settle to the target frame, no timer.
    void setBusyImmediate(bool shouldBeBusy)
    {
        busy = shouldBeBusy;
        setEnabled(! busy);
        busyAmount = busy ? 1.0f : 0.0f;
        spinPhase = 0.0f;
        stopTimer();
        repaint();
    }

    void paintButton(Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        const auto& p = lnf.palette;
        const float radius = 9.0f;

        if (style == Style::accent)
        {
            g.setColour(lnf.accent.withAlpha(0.30f));
            g.fillRoundedRectangle(r.translated(0.0f, 4.0f).reduced(6.0f, 2.0f), radius);
            auto col = lnf.accent;
            if (over) col = col.brighter(0.12f);
            if (down) col = col.darker(0.05f);
            g.setColour(col);
            g.fillRoundedRectangle(r, radius);
            textColour = juce::Colours::white;
        }
        else if (style == Style::danger)
        {
            g.setColour(over ? p.dangerBorder : p.dangerFill);
            g.fillRoundedRectangle(r, radius);
            g.setColour(p.dangerBorder);
            g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
            textColour = p.error;
        }
        else
        {
            g.setColour(over ? p.ghostHover : p.ghostFill);
            g.fillRoundedRectangle(r, radius);
            g.setColour(p.ghostBorder);
            g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
            textColour = p.textPrimary;
        }

        const auto label = getButtonText();
        auto font = lnf.heading(14.0f);
        g.setFont(font);
        const float iconSize = 16.0f;
        const float gap = icon ? 8.0f : 0.0f;
        const float textW = juce::GlyphArrangement::getStringWidth(font, label);
        const float totalW = textW + (icon ? iconSize + gap : 0.0f);
        float x = (r.getWidth() - totalW) * 0.5f;

        const float labelAlpha = 1.0f - busyAmount;
        if (labelAlpha > 0.0f)
        {
            if (icon)
            {
                icon->drawWithin(g, { x, r.getCentreY() - iconSize * 0.5f, iconSize, iconSize },
                                 juce::RectanglePlacement::centred, labelAlpha);
                x += iconSize + gap;
            }
            g.setColour(textColour.withMultipliedAlpha(labelAlpha));
            g.drawText(label, Rectangle<float>(x, 0.0f, textW + 2.0f, r.getHeight()),
                       Justification::centredLeft);
        }

        if (busyAmount > 0.0f)
            drawBusySpinner(g, r, textColour, busyAmount);
    }

    void timerCallback() override
    {
        const float target = busy ? 1.0f : 0.0f;
        const float step = 1.0f / 9.0f; // ~150ms cross-fade between label and spinner
        busyAmount = busyAmount < target ? juce::jmin(target, busyAmount + step)
                                         : juce::jmax(target, busyAmount - step);

        if (busy || busyAmount > 0.0f)
        {
            spinPhase += 1.0f / 66.0f; // ~1.1s per revolution
            if (spinPhase >= 1.0f)
                spinPhase -= 1.0f;
        }
        repaint();

        if (! busy && busyAmount <= 0.0f)
        {
            busyAmount = 0.0f;
            stopTimer();
        }
    }

private:
    void drawBusySpinner(Graphics& g, Rectangle<float> area, Colour colour, float amount)
    {
        const float d = juce::jmin(18.0f, area.getHeight() - 12.0f) * juce::jlimit(0.5f, 1.0f, amount);
        auto sb = Rectangle<float>(0, 0, d, d).withCentre(area.getCentre());
        const float thickness = 2.2f;
        auto ring = sb.reduced(thickness * 0.5f);

        juce::Path track;
        track.addEllipse(ring);
        g.setColour(colour.withMultipliedAlpha(0.22f * amount));
        g.strokePath(track, juce::PathStrokeType(thickness));

        juce::Path arc;
        const float start = spinPhase * kTwoPi;
        arc.addCentredArc(ring.getCentreX(), ring.getCentreY(), ring.getWidth() * 0.5f,
                          ring.getHeight() * 0.5f, 0.0f, start, start + kPi * 0.7f, true);
        g.setColour(colour.withMultipliedAlpha(amount));
        g.strokePath(arc, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    ActivationLookAndFeel& lnf;
    Style style;
    std::unique_ptr<juce::Drawable> icon;
    Colour textColour;
    bool busy = false;
    float busyAmount = 0.0f; // eased 0..1: label <-> spinner
    float spinPhase = 0.0f;
};

class LinkButton : public juce::Button
{
public:
    LinkButton(ActivationLookAndFeel& l, const juce::String& buttonText, Colour c,
               std::unique_ptr<juce::Drawable> ic = {}, Justification just = Justification::centred)
        : juce::Button(buttonText), lnf(l), label(buttonText), colour(c), icon(std::move(ic)), justification(just)
    {
    }

    void paintButton(Graphics& g, bool over, bool /*down*/) override
    {
        auto r = getLocalBounds().toFloat();
        auto font = lnf.body(12.5f);
        g.setFont(font);
        const float iconSize = 14.0f;
        const float gap = icon ? 6.0f : 0.0f;
        const float textW = juce::GlyphArrangement::getStringWidth(font, label);
        const float totalW = textW + (icon ? iconSize + gap : 0.0f);
        float x = justification.testFlags(Justification::left)
                      ? 0.0f
                      : (r.getWidth() - totalW) * 0.5f;
        const auto col = over ? colour.brighter(0.25f) : colour;
        if (icon)
        {
            icon->drawWithin(g, { x, r.getCentreY() - iconSize * 0.5f, iconSize, iconSize },
                             juce::RectanglePlacement::centred, 1.0f);
            x += iconSize + gap;
        }
        g.setColour(col);
        g.drawText(label, Rectangle<float>(x, 0.0f, textW + 2.0f, r.getHeight()),
                   Justification::centredLeft);
    }

private:
    ActivationLookAndFeel& lnf;
    juce::String label;
    Colour colour;
    std::unique_ptr<juce::Drawable> icon;
    Justification justification;
};

//==============================================================================
// Offline response-file target: click to browse, or drag a file onto it.
class DropZone : public juce::Component,
                 public juce::FileDragAndDropTarget
{
public:
    DropZone(ActivationLookAndFeel& l, juce::String fileNoun, juce::String fileExtension)
        : lnf(l), noun(std::move(fileNoun)), extension(std::move(fileExtension))
    {
        emptyIcon = icons::fromStroke(icons::fileQuestion, l.palette.textSecondary, 1.6f);
        fileIcon = icons::fromStroke(icons::checkCircle, l.palette.success, 1.7f);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    std::function<void()> onBrowse;        // clicked
    std::function<void(juce::File)> onFile; // file dropped

    void setFileName(const juce::String& name)
    {
        if (name != fileName)
        {
            fileName = name;
            repaint();
        }
    }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        const bool has = fileName.isNotEmpty();

        g.setColour(dragOver ? lnf.palette.ghostHover : Colour(0x06ffffff));
        g.fillRoundedRectangle(r, 10.0f);

        const auto borderCol = dragOver ? lnf.accent
                               : has    ? lnf.palette.success.withAlpha(0.45f)
                                        : lnf.palette.ghostBorder;
        if (dragOver || has)
        {
            g.setColour(borderCol);
            g.drawRoundedRectangle(r, 10.0f, 1.5f);
        }
        else
        {
            juce::Path outline;
            outline.addRoundedRectangle(r, 10.0f);
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType(1.5f).createDashedStroke(dashed, outline, dashes, 2);
            g.setColour(borderCol);
            g.fillPath(dashed);
        }

        const juce::String line1 = has ? fileName : ("Drop your " + noun + " here");
        const juce::String line2 = has ? juce::String("Click to choose a different file")
                                       : juce::String("or click to browse");
        auto fontMain = has ? lnf.mono(12.5f) : lnf.heading(13.0f);
        auto fontSub = lnf.body(11.5f);

        const float iconS = 22.0f, gap = 10.0f;
        const float avail = r.getWidth() - iconS - gap - 20.0f;
        const float textW = juce::jmin(avail, juce::jmax(juce::GlyphArrangement::getStringWidth(fontMain, line1),
                                                         juce::GlyphArrangement::getStringWidth(fontSub, line2)));
        const float groupW = iconS + gap + textW;
        float gx = r.getCentreX() - groupW * 0.5f;
        const float cy = r.getCentreY();

        if (auto* icon = has ? fileIcon.get() : emptyIcon.get())
            icon->drawWithin(g, { gx, cy - iconS * 0.5f, iconS, iconS }, juce::RectanglePlacement::centred, 1.0f);
        const float tx = gx + iconS + gap;
        g.setColour(has ? lnf.palette.textPrimary : lnf.palette.textBody);
        g.setFont(fontMain);
        g.drawText(line1, Rectangle<float>(tx, cy - 16.0f, textW + 2.0f, 17.0f), Justification::bottomLeft);
        g.setColour(lnf.palette.textSecondary);
        g.setFont(fontSub);
        g.drawText(line2, Rectangle<float>(tx, cy + 1.0f, textW + 2.0f, 15.0f), Justification::topLeft);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (onBrowse != nullptr && getLocalBounds().contains(e.getPosition()))
            onBrowse();
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return matchingFile(files) != juce::File();
    }
    void fileDragEnter(const juce::StringArray& files, int, int) override
    {
        setDragOver(matchingFile(files) != juce::File());
    }
    void fileDragExit(const juce::StringArray&) override { setDragOver(false); }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        setDragOver(false);
        const auto file = matchingFile(files);
        if (file != juce::File() && onFile != nullptr)
            onFile(file);
    }

private:
    void setDragOver(bool o)
    {
        if (o != dragOver)
        {
            dragOver = o;
            repaint();
        }
    }

    // First dropped file matching the accepted extension (any, if unset).
    [[nodiscard]] juce::File matchingFile(const juce::StringArray& files) const
    {
        for (const auto& f : files)
        {
            juce::File file(f);
            if (extension.isEmpty() || file.hasFileExtension(extension))
                return file;
        }
        return {};
    }

    ActivationLookAndFeel& lnf;
    juce::String noun;
    juce::String extension;
    std::unique_ptr<juce::Drawable> emptyIcon, fileIcon;
    juce::String fileName;
    bool dragOver = false;
};

//==============================================================================
// Small round close (X) button shown in the panel's top-right when the flow can
// be dismissed.
class CloseButton : public juce::Button
{
public:
    explicit CloseButton(ActivationLookAndFeel& l) : juce::Button("Close"), lnf(l)
    {
        icon = icons::fromStroke(icons::cross, l.palette.textSecondary, 1.8f);
        setWantsKeyboardFocus(false);
    }

    void paintButton(Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        if (over || down)
        {
            g.setColour(down ? lnf.palette.ghostBorder : lnf.palette.ghostHover);
            g.fillEllipse(r);
        }
        if (icon != nullptr)
            icon->drawWithin(g, r.reduced(8.0f), juce::RectanglePlacement::centred, over ? 1.0f : 0.85f);
    }

private:
    ActivationLookAndFeel& lnf;
    std::unique_ptr<juce::Drawable> icon;
};

//==============================================================================
// "Licensing secured by [mark] moonbase" footer. The Moonbase mark + name are a
// clickable link to moonbase.sh (pointing cursor + brighten on hover).
class MoonbaseBadge : public juce::Component
{
public:
    explicit MoonbaseBadge(ActivationLookAndFeel& l) : lnf(l)
    {
        mark = icons::moonbaseMark(l.palette.textBright);
        lock = icons::fromStroke(icons::lock, l.palette.textMuted, 1.5f);
    }

    void paint(Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(lnf.palette.hairline);
        g.fillRect(area.removeFromTop(1.0f));

        const auto fontA = lnf.body(11.0f);
        const auto fontB = lnf.body(12.0f);
        const juce::String prefix = "Licensing secured by";
        const juce::String name = "moonbase";
        const float w1 = juce::GlyphArrangement::getStringWidth(fontA, prefix);
        const float w2 = juce::GlyphArrangement::getStringWidth(fontB, name);
        const float lockS = 14.0f, markS = 14.0f, g1 = 6.0f, g2 = 8.0f, g3 = 7.0f;
        const float total = lockS + g1 + w1 + g2 + markS + g3 + w2;

        const float cy = area.getCentreY();
        float x = (float) getWidth() * 0.5f - total * 0.5f;

        if (lock != nullptr)
            lock->drawWithin(g, { x, cy - lockS * 0.5f, lockS, lockS }, juce::RectanglePlacement::centred, 1.0f);
        x += lockS + g1;
        g.setColour(lnf.palette.textMuted);
        g.setFont(fontA);
        g.drawText(prefix, Rectangle<float>(x, cy - 9.0f, w1 + 2.0f, 18.0f), Justification::centredLeft);
        x += w1 + g2;

        const float linkX = x; // mark + name form the clickable link
        if (mark != nullptr)
            mark->drawWithin(g, { x, cy - markS * 0.5f, markS, markS }, juce::RectanglePlacement::centred,
                             hovering ? 1.0f : 0.85f);
        x += markS + g3;
        g.setColour(hovering ? lnf.palette.textPrimary : lnf.palette.textBright);
        g.setFont(fontB);
        g.drawText(name, Rectangle<float>(x, cy - 9.0f, w2 + 2.0f, 18.0f), Justification::centredLeft);

        linkArea = Rectangle<float>(linkX, cy - 11.0f, (x + w2) - linkX, 22.0f);
    }

    void mouseMove(const juce::MouseEvent& e) override { updateHover(e.position); }
    void mouseEnter(const juce::MouseEvent& e) override { updateHover(e.position); }
    void mouseExit(const juce::MouseEvent&) override
    {
        setHover(false);
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (linkArea.contains(e.position))
            juce::URL("https://moonbase.sh").launchInDefaultBrowser();
    }

private:
    void updateHover(juce::Point<float> p)
    {
        const bool over = linkArea.contains(p);
        setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        setHover(over);
    }
    void setHover(bool h)
    {
        if (h != hovering)
        {
            hovering = h;
            repaint();
        }
    }

    ActivationLookAndFeel& lnf;
    std::unique_ptr<juce::Drawable> mark, lock;
    juce::Rectangle<float> linkArea;
    bool hovering = false;
};

//==============================================================================
// Screen base
struct ScreenView : juce::Component
{
    ScreenView(ActivationController& c, ActivationLookAndFeel& l) : controller(c), lnf(l) {}
    virtual void refresh() {}

    ActivationController& controller;
    ActivationLookAndFeel& lnf;
    std::function<void()> onCloseRequested;
    int headerRightInset = 0; // space to keep clear at the header's right (close button)
};

//==============================================================================
// Welcome
class WelcomeView : public ScreenView
{
public:
    WelcomeView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        const auto& cfg = controller.config();
        activate = std::make_unique<StyledButton>(l, StyledButton::Style::accent, cfg.activateOnlineText());
        activate->onClick = [this] { controller.beginOnlineActivation(); };
        addAndMakeVisible(*activate);

        if (cfg.enableOffline)
        {
            offline = std::make_unique<LinkButton>(
                l, cfg.activateOfflineText(), l.palette.textSecondary,
                icons::fromStroke(icons::offlineGlobe, l.palette.textSecondary, 1.7f),
                Justification::left);
            offline->onClick = [this] { controller.showOffline(); };
            addAndMakeVisible(*offline);
        }
    }

    void refresh() override { repaint(); } // pick up Error-state status messages

    void paint(Graphics& g) override
    {
        const auto& cfg = controller.config();
        auto r = getLocalBounds();
        drawBrand(g, lnf, cfg.logo.get(), r.removeFromTop(40), cfg.resolvedProductName(), cfg.resolvedManufacturerName(),
                  38.0f, 17.0f);
        r.removeFromTop(24);

        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(27.0f));
        g.drawFittedText(cfg.welcomeTitleText(), r.removeFromTop(34), Justification::topLeft, 1);
        r.removeFromTop(8);

        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(14.0f));
        g.drawFittedText(cfg.welcomeBodyText(), r.removeFromTop(46), Justification::topLeft, 3, 1.0f);

        // Surface activation errors here (the Error screen reuses this view).
        if (controller.statusMessage().isNotEmpty())
        {
            auto errArea = getLocalBounds().removeFromBottom(38);
            if (auto warn = icons::fromStroke(icons::warning, lnf.palette.error, 1.8f))
                warn->drawWithin(g, errArea.removeFromLeft(20).toFloat().withSizeKeepingCentre(16, 16),
                                 juce::RectanglePlacement::centred, 1.0f);
            g.setColour(lnf.palette.error);
            g.setFont(lnf.body(12.5f));
            g.drawFittedText(controller.statusMessage(), errArea, Justification::centredLeft, 2, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop(40 + 24 + 34 + 8 + 46 + 22);
        const int bw = juce::jmin(r.getWidth(), 360);
        activate->setBounds(r.removeFromTop(46).withWidth(bw));
        r.removeFromTop(24);
        if (offline)
            offline->setBounds(r.removeFromTop(20).withWidth(juce::jmin(bw, 280)));
    }

private:
    std::unique_ptr<StyledButton> activate;
    std::unique_ptr<LinkButton> offline;
};

//==============================================================================
// Browser wait
class BrowserWaitView : public ScreenView,
                        private juce::Timer
{
public:
    BrowserWaitView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        cancel = std::make_unique<StyledButton>(l, StyledButton::Style::ghost, "Cancel activation");
        cancel->onClick = [this] { controller.cancelActivation(); };
        addAndMakeVisible(*cancel);
        monitorIcon = icons::fromStroke(icons::monitor, l.palette.textSecondary, 1.6f);
    }

    ~BrowserWaitView() override { stopTimer(); }

    // Self-drive the spinner with a timer so it animates regardless of the
    // shared animation updater — only while this screen is visible.
    void visibilityChanged() override
    {
        if (isVisible())
            startTimerHz(60);
        else
            stopTimer();
    }

    void timerCallback() override
    {
        spinPhase += 1.0f / 72.0f; // ~1.2s per revolution
        if (spinPhase >= 1.0f)
            spinPhase -= 1.0f;
        if (! spinnerRepaint.isEmpty())
            repaint(spinnerRepaint);
        else
            repaint();
    }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds();

        // Reserve the bottom for the cancel button (placed in resized) and the
        // device chip above it, so they never overlap on a shorter window.
        r.removeFromBottom(46); // cancel button row
        r.removeFromBottom(14);
        auto chipRow = r.removeFromBottom(34);
        r.removeFromBottom(18);

        // Spinner at the top of the remaining space.
        drawSpinner(g, r.removeFromTop(juce::jmin(116, juce::jmax(86, r.getHeight() / 2))));

        // Heading + wrapping subtitle fill the middle.
        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(22.0f));
        g.drawText("Waiting for activation", r.removeFromTop(30), Justification::centred);
        r.removeFromTop(8);
        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(14.0f));
        auto sub = r.reduced(4, 0);
        // minimumHorizontalScale = 1.0f forces wrapping instead of squashing glyphs.
        g.drawFittedText("We opened your browser to finish activation. Sign in and confirm this "
                         "device, then we'll pick it up automatically.",
                         sub.getX(), sub.getY(), sub.getWidth(), sub.getHeight(),
                         Justification::centredTop, 5, 1.0f);

        drawChip(g, chipRow);
    }

    void resized() override
    {
        cancel->setBounds(getLocalBounds().removeFromBottom(46)
                              .withSizeKeepingCentre(juce::jmin(getWidth(), 320), 46));
    }

private:
    void drawSpinner(Graphics& g, Rectangle<int> area)
    {
        const float diameter = 58.0f;
        auto sb = Rectangle<float>(0, 0, diameter, diameter)
                      .withCentre({ (float) getWidth() * 0.5f, (float) area.getCentreY() });
        spinnerRepaint = sb.expanded(10.0f).getSmallestIntegerContainer();

        g.setColour(lnf.accent.withAlpha(0.16f));
        g.fillEllipse(sb.expanded(7.0f));

        const float thickness = 4.0f;
        auto ring = sb.reduced(thickness * 0.5f + 1.0f);
        juce::Path track;
        track.addEllipse(ring);
        g.setColour(Colour(0x26ffffff));
        g.strokePath(track, juce::PathStrokeType(thickness));

        juce::Path arc;
        const float start = spinPhase * kTwoPi;
        arc.addCentredArc(ring.getCentreX(), ring.getCentreY(), ring.getWidth() * 0.5f,
                          ring.getHeight() * 0.5f, 0.0f, start, start + kPi * 0.6f, true);
        g.setColour(Colour(0xff82cef1));
        g.strokePath(arc, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    void drawChip(Graphics& g, Rectangle<int> row)
    {
        const auto chipText = controller.deviceLabel();
        auto chipFont = lnf.mono(12.5f);
        const float tw = juce::GlyphArrangement::getStringWidth(chipFont, chipText);
        auto chip = Rectangle<float>(0, 0, juce::jmin((float) getWidth(), tw + 52.0f), 32.0f)
                        .withCentre({ (float) getWidth() * 0.5f, (float) row.getCentreY() });
        g.setColour(Colour(0x0affffff));
        g.fillRoundedRectangle(chip, 16.0f);
        g.setColour(lnf.palette.ghostBorder);
        g.drawRoundedRectangle(chip, 16.0f, 1.0f);
        if (monitorIcon != nullptr)
            monitorIcon->drawWithin(g, chip.removeFromLeft(34).reduced(9.0f),
                                    juce::RectanglePlacement::centred, 1.0f);
        g.setColour(lnf.palette.textBright);
        g.setFont(chipFont);
        g.drawText(chipText, chip, Justification::centredLeft);
    }

    std::unique_ptr<StyledButton> cancel;
    std::unique_ptr<juce::Drawable> monitorIcon;
    Rectangle<int> spinnerRepaint;
    float spinPhase = 0.0f;
};

//==============================================================================
// Success
class SuccessView : public ScreenView
{
public:
    SuccessView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        open = std::make_unique<StyledButton>(l, StyledButton::Style::accent, "Open " + c.config().resolvedProductName());
        open->onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible(*open);

        details = std::make_unique<LinkButton>(l, "View license details", l.palette.link);
        details->onClick = [this] { controller.showDetails(); };
        addAndMakeVisible(*details);

        check = icons::fromStroke(icons::check, l.palette.success, 2.4f);
    }

    void setPop(float p) { pop = p; repaint(); }
    void refresh() override
    {
        open->setButtonText("Open " + controller.config().resolvedProductName());
        repaint();
    }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds();

        // Reserve the bottom for the buttons (placed in resized) and anchor the
        // license card just above them, so the card never slides under the
        // buttons when the window is short.
        r.removeFromBottom(20 + 14 + 46); // details link + gap + open button
        r.removeFromBottom(14);           // gap above the card
        auto cardRow = r.removeFromBottom(86);
        r.removeFromBottom(12);           // gap below the subtitle

        // Check badge, scaling down if vertical space is tight.
        const int badgeH = juce::jlimit(44, 96, r.getHeight() - 76);
        auto badge = r.removeFromTop(badgeH);
        const float d = juce::jmin(66.0f, (float) badge.getHeight() - 8.0f);
        auto circle = Rectangle<float>(0, 0, d, d)
                          .withCentre({ (float) getWidth() * 0.5f, (float) badge.getCentreY() });
        auto scaled = circle.withSizeKeepingCentre(circle.getWidth() * pop, circle.getHeight() * pop);
        g.setColour(lnf.palette.successFill);
        g.fillEllipse(scaled);
        g.setColour(lnf.palette.successBorder);
        g.drawEllipse(scaled, 1.0f);
        if (check && pop > 0.3f)
            check->drawWithin(g, scaled.reduced(scaled.getWidth() * 0.3f),
                              juce::RectanglePlacement::centred, juce::jlimit(0.0f, 1.0f, pop));

        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(25.0f));
        g.drawText("You're all set", r.removeFromTop(32), Justification::centred);
        r.removeFromTop(6);
        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(14.0f));
        g.drawFittedText(controller.config().resolvedProductName()
                             + " is activated on this machine. Your license is linked to your account.",
                         r.getX(), r.getY(), r.getWidth(), r.getHeight(), Justification::centredTop, 2, 1.0f);

        // Mini card (anchored above the buttons).
        auto card = cardRow.withSizeKeepingCentre(juce::jmin(380, getWidth()),
                                                  juce::jmin(86, cardRow.getHeight()));
        g.setColour(Colour(0x06ffffff));
        g.fillRoundedRectangle(card.toFloat(), 12.0f);
        g.setColour(lnf.palette.panelBorder);
        g.drawRoundedRectangle(card.toFloat(), 12.0f, 1.0f);
        auto inner = card.reduced(18, 14);
        drawCardRow(g, inner.removeFromTop(28), "Plan", planText());
        g.setColour(lnf.palette.hairline);
        g.fillRect(inner.removeFromTop(1));
        drawCardRow(g, inner.removeFromTop(28), "Licensed to", licensedToText());
    }

    void resized() override
    {
        auto r = getLocalBounds();
        details->setBounds(r.removeFromBottom(20));
        r.removeFromBottom(14);
        open->setBounds(r.removeFromBottom(46).withSizeKeepingCentre(juce::jmin(320, getWidth()), 46));
    }

private:
    juce::String planText() const
    {
        if (auto& lic = controller.license())
            return controller.config().resolvedProductName() + kMidDot
                   + (lic->trial ? juce::String("Trial") : juce::String("Full license"));
        return controller.config().resolvedProductName();
    }
    juce::String licensedToText() const
    {
        if (auto& lic = controller.license())
            return juce::String(lic->issued_to.email);
        return {};
    }
    void drawCardRow(Graphics& g, Rectangle<int> row, const juce::String& key, const juce::String& value)
    {
        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(12.5f));
        g.drawText(key, row, Justification::centredLeft);
        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(13.0f));
        g.drawText(value, row, Justification::centredRight);
    }

    std::unique_ptr<StyledButton> open;
    std::unique_ptr<LinkButton> details;
    std::unique_ptr<juce::Drawable> check;
    float pop = 1.0f;
};

//==============================================================================
// Offline
class OfflineView : public ScreenView
{
public:
    OfflineView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        back = std::make_unique<LinkButton>(l, "Back", l.palette.textSecondary,
                                            icons::fromStroke(icons::back, l.palette.textSecondary, 2.0f),
                                            Justification::left);
        back->onClick = [this] { controller.showWelcome(); };
        addAndMakeVisible(*back);

        saveMachine = std::make_unique<StyledButton>(
            l, StyledButton::Style::ghost, u8("Save machine file\xe2\x80\xa6"),
            icons::fromStroke(icons::fileDown, Colour(0xff82cef1), 1.6f));
        saveMachine->onClick = [this] { chooseMachineFileLocation(); };
        addAndMakeVisible(*saveMachine);

        dropZone = std::make_unique<DropZone>(l, "license file", "mb");
        dropZone->onBrowse = [this] { chooseLicenseFile(); };
        dropZone->onFile = [this](juce::File f) { controller.setOfflineResponse(f); };
        addAndMakeVisible(*dropZone);

        urlLink = std::make_unique<LinkButton>(
            l, controller.config().activationUrlDisplay(), l.palette.link,
            icons::fromStroke(icons::externalLink, l.palette.link, 1.6f), Justification::left);
        urlLink->onClick = [this] { controller.config().activationUrlResolved().launchInDefaultBrowser(); };
        urlLink->setMouseCursor(juce::MouseCursor::PointingHandCursor);
        addAndMakeVisible(*urlLink);

        activate = std::make_unique<StyledButton>(l, StyledButton::Style::accent, "Activate offline");
        activate->onClick = [this] { controller.activateOffline(); };
        addAndMakeVisible(*activate);
    }

    void refresh() override
    {
        dropZone->setFileName(controller.hasOfflineResponse() ? controller.offlineResponseName()
                                                              : juce::String());
        repaint();
    }

    void paint(Graphics& g) override
    {
        const auto l = computeLayout();

        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(23.0f));
        g.drawText("Offline activation", l.heading, Justification::topLeft);

        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(14.0f));
        g.drawFittedText("Save your machine file, exchange it for a license file on an "
                         "internet-connected device, then load that file back here.",
                         l.subtitle.getX(), l.subtitle.getY(), l.subtitle.getWidth(),
                         l.subtitle.getHeight(), Justification::topLeft, 2, 1.0f);

        drawStep(g, l.step1, 1, "Save your machine file");
        drawStep(g, l.step2, 2, "Add the license file");

        if (controller.offlineError().isNotEmpty() && ! l.error.isEmpty())
        {
            auto line = l.error;
            if (auto w = icons::fromStroke(icons::warning, lnf.palette.error, 1.8f))
                w->drawWithin(g, line.removeFromLeft(20).toFloat().withSizeKeepingCentre(15.0f, 15.0f),
                              juce::RectanglePlacement::centred, 1.0f);
            line.removeFromLeft(4);
            g.setColour(lnf.palette.error);
            g.setFont(lnf.body(12.5f));
            g.drawFittedText(controller.offlineError(), line.getX(), line.getY(), line.getWidth(),
                             line.getHeight(), Justification::centredLeft, 2, 1.0f);
        }
    }

    void resized() override
    {
        const auto l = computeLayout();
        back->setBounds(l.back.withWidth(juce::jmin(90, getWidth())));
        urlLink->setBounds(l.urlLink);
        saveMachine->setBounds(l.saveBtn);
        dropZone->setBounds(l.dropZone);
        activate->setBounds(l.activate);
    }

private:
    struct OfflineLayout
    {
        Rectangle<int> back, heading, subtitle, urlLink, step1, saveBtn, step2, dropZone, error, activate;
    };

    // Single source of truth for the layout so paint() and resized() never
    // disagree. Flows top-down with the Activate button (and any error line)
    // reserved at the bottom; compresses gracefully on shorter windows.
    OfflineLayout computeLayout()
    {
        auto r = getLocalBounds();
        OfflineLayout l;
        l.back = r.removeFromTop(22);
        r.removeFromTop(2);
        l.heading = r.removeFromTop(26);
        r.removeFromTop(2);
        l.subtitle = r.removeFromTop(34);
        r.removeFromTop(4);
        l.urlLink = r.removeFromTop(18);
        r.removeFromTop(8);

        l.activate = r.removeFromBottom(44);
        r.removeFromBottom(8);
        if (controller.offlineError().isNotEmpty())
        {
            l.error = r.removeFromBottom(30);
            r.removeFromBottom(6);
        }

        l.step1 = r.removeFromTop(20);
        r.removeFromTop(4);
        l.saveBtn = r.removeFromTop(40);
        r.removeFromTop(10);
        l.step2 = r.removeFromTop(20);
        r.removeFromTop(4);
        l.dropZone = r.removeFromTop(52);
        return l;
    }

    void drawStep(Graphics& g, Rectangle<int> row, int number, const juce::String& text)
    {
        auto badge = row.removeFromLeft(20).withSizeKeepingCentre(20, 20).toFloat();
        g.setColour(lnf.accent);
        g.fillEllipse(badge);
        g.setColour(juce::Colours::white);
        g.setFont(lnf.heading(11.0f));
        g.drawText(juce::String(number), badge, Justification::centred);
        row.removeFromLeft(10);
        g.setColour(lnf.palette.textBright);
        g.setFont(lnf.heading(12.5f));
        g.drawText(text, row, Justification::centredLeft);
    }

    void chooseMachineFileLocation()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Save machine file",
            juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                .getChildFile(controller.config().resolvedProductName() + " machine file.dt"),
            "*.dt");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [this](const juce::FileChooser& fc)
                             {
                                 auto file = fc.getResult();
                                 if (file != juce::File())
                                     controller.saveOfflineRequest(file);
                             });
    }

    void chooseLicenseFile()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Choose license file",
            juce::File::getSpecialLocation(juce::File::userDesktopDirectory), "*.mb");
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc)
                             {
                                 auto file = fc.getResult();
                                 if (file != juce::File())
                                     controller.setOfflineResponse(file);
                             });
    }

    std::unique_ptr<LinkButton> back, urlLink;
    std::unique_ptr<StyledButton> saveMachine, activate;
    std::unique_ptr<DropZone> dropZone;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
// Trial
// Trial feature rows, evenly spaced. Sits inside a Viewport so it scrolls when
// the list is longer than the available area; centres vertically when it fits.
class TrialFeatureList : public juce::Component
{
public:
    TrialFeatureList(ActivationController& c, ActivationLookAndFeel& l) : controller(c), lnf(l) {}

    static constexpr int rowHeight = 28;

    [[nodiscard]] int contentHeight() const
    {
        return (int) controller.config().trialFeatures.size() * rowHeight;
    }

    void paint(Graphics& g) override
    {
        const auto& feats = controller.config().trialFeatures;
        if (feats.empty())
            return;
        // Centre the block when there is slack (it fits); top-align when it
        // overflows (the viewport scrolls). Either way the rows are even.
        const int block = (int) feats.size() * rowHeight;
        int y = juce::jmax(0, (getHeight() - block) / 2);
        for (const auto& f : feats)
        {
            drawFeature(g, { 0, y, getWidth(), rowHeight }, f);
            y += rowHeight;
        }
    }

private:
    void drawFeature(Graphics& g, Rectangle<int> row, const TrialFeature& f)
    {
        auto iconArea = row.removeFromLeft(17).withSizeKeepingCentre(17, 17).toFloat();
        const auto colour = f.included ? lnf.palette.success : lnf.palette.textMuted;
        if (auto ic = icons::fromStroke(f.included ? icons::check : icons::cross, colour, 2.0f))
            ic->drawWithin(g, iconArea, juce::RectanglePlacement::centred, 1.0f);
        row.removeFromLeft(10);
        g.setColour(f.included ? lnf.palette.textBody : lnf.palette.textMuted);
        g.setFont(lnf.body(13.0f));
        g.drawText(f.label, row, Justification::centredLeft);
    }

    ActivationController& controller;
    ActivationLookAndFeel& lnf;
};

class TrialView : public ScreenView
{
public:
    TrialView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        unlock = std::make_unique<StyledButton>(l, StyledButton::Style::accent, "Unlock full version",
                                                icons::fromStroke(icons::lock, juce::Colours::white, 1.8f));
        unlock->onClick = [this] { controller.beginOnlineActivation(); };
        addAndMakeVisible(*unlock);

        cont = std::make_unique<LinkButton>(l, "Continue trial", l.palette.textSecondary);
        cont->onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible(*cont);

        featureList = std::make_unique<TrialFeatureList>(c, l);
        featuresViewport.setViewedComponent(featureList.get(), false);
        featuresViewport.setScrollBarsShown(true, false); // vertical only, when needed
        featuresViewport.setScrollBarThickness(8);
        addAndMakeVisible(featuresViewport);
    }

    void refresh() override { repaint(); }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds();
        // Reserve the bottom for the buttons so the feature list never slides
        // under them on a short window.
        r.removeFromBottom(46 + 14 + 20 + 14); // unlock + gap + continue + gap

        auto headerRow = r.removeFromTop(40);
        auto pill = headerRow.removeFromRight(150);
        drawBrand(g, lnf, controller.config().logo.get(), headerRow, controller.config().resolvedProductName(),
                  controller.config().resolvedManufacturerName(), 34.0f, 15.0f);

        const int days = controller.trialDaysRemaining();
        auto pillText = u8("Trial") + kMidDot + juce::String(days) + (days == 1 ? " day left" : " days left");
        auto pf = lnf.heading(10.5f);
        const float pw = juce::GlyphArrangement::getStringWidth(pf, pillText) + 22.0f;
        auto pb = Rectangle<float>(0, 0, pw, 22).withCentre(
            { (float) (pill.getRight() - headerRightInset) - pw * 0.5f, (float) headerRow.getCentreY() });
        g.setColour(lnf.palette.trial);
        g.fillRoundedRectangle(pb, 11.0f);
        g.setColour(Colour(0xff131519));
        g.setFont(pf);
        g.drawText(pillText.toUpperCase(), pb, Justification::centred);

        r.removeFromTop(26);
        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(24.0f));
        g.drawText("You're on a free trial", r.removeFromTop(30), Justification::topLeft);
        r.removeFromTop(8);
        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(14.0f));
        const int total = controller.config().trialLengthDays;
        {
            auto subtitle = r.removeFromTop(40);
            g.drawFittedText(juce::String(days) + " of " + juce::String(total)
                                 + " days remaining. Unlock the full version any time to keep everything.",
                             subtitle.getX(), subtitle.getY(), subtitle.getWidth(), subtitle.getHeight(),
                             Justification::topLeft, 2, 1.0f);
        }

        // progress bar
        r.removeFromTop(14);
        auto bar = r.removeFromTop(6);
        g.setColour(Colour(0x12ffffff));
        g.fillRoundedRectangle(bar.toFloat(), 3.0f);
        const float frac = total > 0 ? juce::jlimit(0.0f, 1.0f, (float) days / (float) total) : 0.0f;
        auto fill = bar.toFloat().withWidth(bar.getWidth() * frac);
        g.setGradientFill(juce::ColourGradient(lnf.palette.trial, fill.getX(), 0,
                                               Colour(0xfff5c542), fill.getRight(), 0, false));
        g.fillRoundedRectangle(fill, 3.0f);

        // The feature list itself is drawn by the scrollable viewport (see
        // resized() / featureArea()).
    }

    void resized() override
    {
        auto r = getLocalBounds();
        cont->setBounds(r.removeFromBottom(20));
        r.removeFromBottom(14);
        unlock->setBounds(r.removeFromBottom(46));

        const auto area = featureArea();
        featuresViewport.setBounds(area);
        const int content = featureList->contentHeight();
        const bool scrolls = content > area.getHeight();
        // Fits -> size to the viewport so the rows centre; overflows -> size to
        // the content (minus the scrollbar gutter) so the viewport scrolls.
        featureList->setSize(scrolls ? juce::jmax(0, area.getWidth() - 10) : area.getWidth(),
                             scrolls ? content : area.getHeight());
    }

private:
    // The middle band between the top copy/progress bar and the bottom buttons.
    // The top/bottom amounts mirror the fixed layout in paint() / resized().
    [[nodiscard]] Rectangle<int> featureArea() const
    {
        auto a = getLocalBounds();
        a.removeFromTop(40 + 26 + 30 + 8 + 40 + 14 + 6); // brand .. progress bar
        a.removeFromTop(20);                              // gap below the bar
        a.removeFromBottom(46 + 14 + 20);                 // unlock + gap + continue
        return a;
    }

    juce::Viewport featuresViewport;
    std::unique_ptr<TrialFeatureList> featureList;
    std::unique_ptr<StyledButton> unlock;
    std::unique_ptr<LinkButton> cont;
};

//==============================================================================
// Trial expired (locked: the plugin stays bypassed until activated)
class ExpiredView : public ScreenView
{
public:
    ExpiredView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        unlock = std::make_unique<StyledButton>(l, StyledButton::Style::accent, "Unlock full version",
                                                icons::fromStroke(icons::lock, juce::Colours::white, 1.8f));
        unlock->onClick = [this] { controller.beginOnlineActivation(); };
        addAndMakeVisible(*unlock);

        offline = std::make_unique<LinkButton>(l, "Activate offline instead", l.palette.textSecondary);
        offline->onClick = [this] { controller.showOffline(); };
        addChildComponent(*offline); // visibility set in refresh()

        warnIcon = icons::fromStroke(icons::warning, l.palette.error, 1.8f);
    }

    void refresh() override
    {
        offline->setVisible(controller.config().enableOffline);
        repaint();
    }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds();
        const bool withOffline = controller.config().enableOffline;

        // Reserve the bottom for the buttons so content never slides under them.
        r.removeFromBottom(46); // unlock button
        if (withOffline)
            r.removeFromBottom(14 + 20); // gap + offline link

        // Header: a muted brand lockup + a red "Trial expired" pill.
        auto headerRow = r.removeFromTop(40);
        auto pillSlot = headerRow.removeFromRight(150);
        drawBrand(g, lnf, controller.config().logo.get(), headerRow,
                  controller.config().resolvedProductName(), controller.config().resolvedManufacturerName(),
                  34.0f, 15.0f, /*muted*/ true);
        drawExpiredPill(g, pillSlot, headerRow.getCentreY());

        r.removeFromTop(26);
        g.setColour(lnf.palette.textPrimary);
        g.setFont(lnf.heading(24.0f));
        g.drawText("Your trial has ended", r.removeFromTop(30), Justification::topLeft);
        r.removeFromTop(8);

        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(14.0f));
        {
            auto subtitle = r.removeFromTop(40);
            g.drawFittedText(bodyText(), subtitle.getX(), subtitle.getY(), subtitle.getWidth(),
                             subtitle.getHeight(), Justification::topLeft, 2, 1.0f);
        }

        // Full, red progress bar (the trial bar at 100%).
        r.removeFromTop(14);
        auto bar = r.removeFromTop(6);
        g.setColour(Colour(0x12ffffff));
        g.fillRoundedRectangle(bar.toFloat(), 3.0f);
        g.setGradientFill(juce::ColourGradient(Colour(0xffb9444c), (float) bar.getX(), 0.0f,
                                               Colour(0xffdc5050), (float) bar.getRight(), 0.0f, false));
        g.fillRoundedRectangle(bar.toFloat(), 3.0f);

        // Red "audio is bypassed" callout.
        r.removeFromTop(22);
        auto callout = r.removeFromTop(juce::jmin(66, r.getHeight()));
        g.setColour(Colour(0x12dc5050));
        g.fillRoundedRectangle(callout.toFloat(), 10.0f);
        g.setColour(Colour(0x38dc5050));
        g.drawRoundedRectangle(callout.toFloat(), 10.0f, 1.0f);
        auto inner = callout.reduced(15, 12);
        auto iconArea = inner.removeFromLeft(18).withSizeKeepingCentre(18, 18).toFloat();
        if (warnIcon != nullptr)
            warnIcon->drawWithin(g, iconArea, juce::RectanglePlacement::centred, 1.0f);
        inner.removeFromLeft(11);
        g.setColour(lnf.palette.textBody);
        g.setFont(lnf.body(13.0f));
        g.drawFittedText("Audio processing is bypassed. Existing projects still load, but "
                             + controller.config().resolvedProductName()
                             + " won't affect your sound until you activate.",
                         inner.getX(), inner.getY(), inner.getWidth(), inner.getHeight(),
                         Justification::centredLeft, 3, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (controller.config().enableOffline)
        {
            offline->setBounds(r.removeFromBottom(20));
            r.removeFromBottom(14);
        }
        unlock->setBounds(r.removeFromBottom(46));
    }

private:
    juce::String bodyText() const
    {
        const int len = controller.config().trialLengthDays;
        juce::String date;
        if (auto& lic = controller.expiredTrial(); lic && lic->expires_at)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                lic->expires_at->time_since_epoch()).count();
            date = juce::Time((juce::int64) ms).toString(true, false);
        }
        juce::String s("Your ");
        if (len > 0)
            s << len << "-day ";
        s << "trial of " << controller.config().resolvedProductName();
        if (date.isNotEmpty())
            s << " ended on " << date;
        s << ". Unlock the full version to keep using the plugin.";
        return s;
    }

    void drawExpiredPill(Graphics& g, Rectangle<int> slot, int centreY)
    {
        const juce::String label = juce::String("Trial expired").toUpperCase();
        auto pf = lnf.heading(10.5f);
        const float tw = juce::GlyphArrangement::getStringWidth(pf, label);
        const float leftPad = 11.0f, dotD = 6.0f, gap = 7.0f, rightPad = 12.0f;
        const float pw = leftPad + dotD + gap + tw + rightPad;
        auto pb = Rectangle<float>(0, 0, pw, 22.0f).withCentre(
            { (float) (slot.getRight() - headerRightInset) - pw * 0.5f, (float) centreY });
        g.setColour(Colour(0x1edc5050));
        g.fillRoundedRectangle(pb, 11.0f);
        g.setColour(Colour(0x52dc5050));
        g.drawRoundedRectangle(pb, 11.0f, 1.0f);
        g.setColour(lnf.palette.error);
        g.fillEllipse(pb.getX() + leftPad, pb.getCentreY() - dotD * 0.5f, dotD, dotD);
        g.setFont(pf);
        g.drawText(label,
                   Rectangle<float>(pb.getX() + leftPad + dotD + gap, pb.getY(), tw + 2.0f, pb.getHeight()),
                   Justification::centredLeft);
    }

    std::unique_ptr<StyledButton> unlock;
    std::unique_ptr<LinkButton> offline;
    std::unique_ptr<juce::Drawable> warnIcon;
};

//==============================================================================
// License details
class DetailsView : public ScreenView
{
public:
    DetailsView(ActivationController& c, ActivationLookAndFeel& l) : ScreenView(c, l)
    {
        deactivate = std::make_unique<StyledButton>(l, StyledButton::Style::danger,
                                                    "Deactivate this device");
        deactivate->onClick = [this] { controller.deactivate(); };
        addAndMakeVisible(*deactivate);
    }

    void refresh() override
    {
        // Offline licenses are permanent: no seats, no server-side revoke.
        deactivate->setVisible(! isOffline());
        if (controller.config().reduceMotion)
            deactivate->setBusyImmediate(controller.isBusy());
        else
            deactivate->setBusy(controller.isBusy());
        repaint();
    }

    // If we slide away mid-revoke (e.g. it succeeded and we return to Welcome),
    // refresh() won't fire again here, so clear the spinner as we leave.
    void visibilityChanged() override
    {
        if (! isVisible())
            deactivate->setBusy(false);
    }

    void paint(Graphics& g) override
    {
        auto r = getLocalBounds();
        const bool offline = isOffline();

        // Reserve the bottom: the seat / deactivate box for online licenses, or a
        // short permanence note for offline ones (which have no seats/revoke).
        Rectangle<int> bottomBox;
        if (offline)
        {
            bottomBox = r.removeFromBottom(22);
            r.removeFromBottom(14);
        }
        else
        {
            bottomBox = r.removeFromBottom(58);
            r.removeFromBottom(14);
        }

        auto headerRow = r.removeFromTop(40);
        drawBrand(g, lnf, controller.config().logo.get(), headerRow, controller.config().resolvedProductName(),
                  controller.config().resolvedManufacturerName(), 34.0f, 15.0f);
        drawActivePill(g, headerRow);
        r.removeFromTop(22);

        // The busy state is shown by the inline spinner in the deactivate button,
        // so only surface a real error message here.
        if (controller.statusMessage().isNotEmpty() && ! controller.isBusy())
        {
            auto msg = r.removeFromBottom(20);
            r.removeFromBottom(8);
            g.setColour(lnf.palette.error);
            g.setFont(lnf.body(12.5f));
            g.drawFittedText(controller.statusMessage(), msg.getX(), msg.getY(), msg.getWidth(),
                             msg.getHeight(), Justification::topLeft, 2, 1.0f);
        }

        // Info card fills the remaining middle, capped at its natural height.
        auto card = r.removeFromTop(juce::jmin(5 * 38, r.getHeight()));
        g.setColour(Colour(0x06ffffff));
        g.fillRoundedRectangle(card.toFloat(), 12.0f);
        g.setColour(lnf.palette.panelBorder);
        g.drawRoundedRectangle(card.toFloat(), 12.0f, 1.0f);

        auto inner = card.reduced(16, 0);
        if (auto& lic = controller.license())
        {
            drawRow(g, inner.removeFromTop(38), "Licensed to",
                    lic->issued_to.name.empty() ? lic->issued_to.email : juce::String(lic->issued_to.name));
            divider(g, inner);
            drawRow(g, inner.removeFromTop(38), "Email", juce::String(lic->issued_to.email));
            divider(g, inner);
            drawRow(g, inner.removeFromTop(38), "Plan",
                    juce::String(lic->licensed_product.name) + (lic->trial ? kMidDot + "trial" : kMidDot + "license"));
            divider(g, inner);
            drawRow(g, inner.removeFromTop(38), "Activation", juce::String(moonbase::to_string(lic->method)));
            divider(g, inner);
            drawRow(g, inner.removeFromTop(38), "Expires", expiryText(*lic));
        }

        if (offline)
        {
            auto& lic = controller.license();
            const bool hasExpiry = lic && lic->expires_at.has_value();
            g.setColour(lnf.palette.textSecondary);
            g.setFont(lnf.body(12.5f));
            g.drawText(hasExpiry
                           ? "Activated offline" + kMidDot + "valid until " + expiryText(*lic)
                           : "Activated offline" + kMidDot + "this license is permanent",
                       bottomBox, Justification::centred);
        }
        else
        {
            drawSeatBox(g, bottomBox);
        }
    }

    void resized() override
    {
        // Always position it (visibility is driven by refresh() on state change).
        auto content = getLocalBounds().removeFromBottom(58).reduced(16, 12);
        const int bw = juce::jmin(180, content.getWidth() / 2);
        deactivate->setBounds(content.removeFromRight(bw).withSizeKeepingCentre(bw, 32));
    }

private:
    struct Seats { int used; int total; };

    [[nodiscard]] bool isOffline() const
    {
        auto& lic = controller.license();
        return lic && lic->method == moonbase::activation_method::offline;
    }

    [[nodiscard]] std::optional<Seats> seats() const
    {
        auto& lic = controller.license();
        if (! lic || ! lic->seat_count || *lic->seat_count <= 0)
            return std::nullopt;
        const int total = (int) *lic->seat_count;
        const int used = lic->seats_used ? (int) *lic->seats_used : 0;
        return Seats{ juce::jlimit(0, total, used), total };
    }

    void drawSeatBox(Graphics& g, Rectangle<int> box)
    {
        g.setColour(Colour(0x06ffffff));
        g.fillRoundedRectangle(box.toFloat(), 12.0f);
        g.setColour(lnf.palette.panelBorder);
        g.drawRoundedRectangle(box.toFloat(), 12.0f, 1.0f);

        auto inner = box.reduced(16, 12);
        const int bw = juce::jmin(180, inner.getWidth() / 2);
        inner.removeFromRight(bw + 14); // make room for the deactivate button

        if (auto s = seats())
        {
            g.setColour(lnf.palette.textBright);
            g.setFont(lnf.heading(12.5f));
            const auto label = juce::String("Activations") + kMidDot + juce::String(s->used) + " of "
                             + juce::String(s->total) + (s->total == 1 ? " device" : " devices");
            g.drawText(label, inner.removeFromTop(18), Justification::centredLeft);
            inner.removeFromTop(8);
            drawSeatBar(g, inner.removeFromTop(6), s->used, s->total);
        }
        else
        {
            g.setColour(lnf.palette.textSecondary);
            g.setFont(lnf.body(12.5f));
            g.drawText("This device's activation", inner, Justification::centredLeft);
        }
    }

    void drawSeatBar(Graphics& g, Rectangle<int> row, int used, int total)
    {
        auto r = row.toFloat();
        if (total <= 6)
        {
            const float gap = 6.0f;
            const float segW = juce::jmin(36.0f, (r.getWidth() - gap * (float) (total - 1)) / (float) total);
            float x = r.getX();
            for (int i = 0; i < total; ++i)
            {
                g.setColour(i < used ? lnf.accent : Colour(0x1affffff));
                g.fillRoundedRectangle(x, r.getY(), segW, r.getHeight(), r.getHeight() * 0.5f);
                x += segW + gap;
            }
        }
        else
        {
            g.setColour(Colour(0x1affffff));
            g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
            const float frac = juce::jlimit(0.0f, 1.0f, (float) used / (float) total);
            g.setColour(lnf.accent);
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * frac), r.getHeight() * 0.5f);
        }
    }

    void drawActivePill(Graphics& g, Rectangle<int> headerRow)
    {
        const juce::String label = "Active";
        auto font = lnf.heading(11.0f);
        const float tw = juce::GlyphArrangement::getStringWidth(font, label);
        const float leftPad = 11.0f, dotD = 6.0f, gap = 7.0f, rightPad = 12.0f;
        const float pw = leftPad + dotD + gap + tw + rightPad;
        auto pb = Rectangle<float>(0, 0, pw, 24.0f)
                      .withCentre({ (float) (headerRow.getRight() - headerRightInset) - pw * 0.5f,
                                    (float) headerRow.getCentreY() });
        g.setColour(lnf.palette.successFill);
        g.fillRoundedRectangle(pb, 12.0f);
        g.setColour(lnf.palette.successBorder);
        g.drawRoundedRectangle(pb, 12.0f, 1.0f);
        g.setColour(lnf.palette.success);
        g.fillEllipse(pb.getX() + leftPad, pb.getCentreY() - dotD * 0.5f, dotD, dotD);
        g.setFont(font);
        g.drawText(label, Rectangle<float>(pb.getX() + leftPad + dotD + gap, pb.getY(), tw + 2.0f, pb.getHeight()),
                   Justification::centredLeft);
    }

    static juce::String expiryText(const moonbase::license& lic)
    {
        if (! lic.expires_at)
            return "Never";
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            lic.expires_at->time_since_epoch())
                            .count();
        return juce::Time((juce::int64) ms).toString(true, false);
    }
    void divider(Graphics& g, Rectangle<int>& area)
    {
        g.setColour(lnf.palette.hairline);
        g.fillRect(area.removeFromTop(1));
    }
    void drawRow(Graphics& g, Rectangle<int> row, const juce::String& key, const juce::String& value)
    {
        g.setColour(lnf.palette.textSecondary);
        g.setFont(lnf.body(12.5f));
        g.drawText(key, row, Justification::centredLeft);
        g.setColour(lnf.palette.textBody);
        g.setFont(lnf.heading(13.0f));
        g.drawText(value, row, Justification::centredRight);
    }

    std::unique_ptr<StyledButton> deactivate;
};

//==============================================================================
// Impl
struct ActivationComponent::Impl : public juce::ChangeListener,
                                   private juce::Timer
{
    Impl(ActivationComponent& o, ActivationConfig cfg)
        : owner(o), lnf(cfg.accent), controller(std::move(cfg))
    {
        // The views live in a clipping host so directional slides are masked to
        // the content area instead of bleeding over the panel chrome.
        screenHost.setInterceptsMouseClicks(false, true);
        owner.addAndMakeVisible(screenHost);

        welcome = std::make_unique<WelcomeView>(controller, lnf);
        browser = std::make_unique<BrowserWaitView>(controller, lnf);
        success = std::make_unique<SuccessView>(controller, lnf);
        offline = std::make_unique<OfflineView>(controller, lnf);
        trial = std::make_unique<TrialView>(controller, lnf);
        expired = std::make_unique<ExpiredView>(controller, lnf);
        details = std::make_unique<DetailsView>(controller, lnf);

        for (auto* v : views())
        {
            screenHost.addChildComponent(*v);
            v->onCloseRequested = [this] { if (owner.onClose) owner.onClose(); };
        }

        closeButton = std::make_unique<CloseButton>(lnf);
        closeButton->onClick = [this] { if (owner.onClose) owner.onClose(); };
        owner.addChildComponent(*closeButton); // shown only when owner.onClose is set

        moonbaseBadge = std::make_unique<MoonbaseBadge>(lnf);
        owner.addChildComponent(*moonbaseBadge); // shown only when config.showMoonbaseBadge

        buildAnimators();
        // Drive the JUCE 8 animators from a timer rather than a
        // VBlankAnimatorUpdater: the latter did not deliver ticks reliably for a
        // freshly-shown plugin/app window. update() uses the hi-res clock.
        startTimerHz(60);
        controller.addChangeListener(this);
        controller.start();
    }

    ~Impl() override
    {
        stopTimer();
        controller.removeChangeListener(this);
    }

    void timerCallback() override { updater.update(); }

    std::vector<ScreenView*> views()
    {
        return { welcome.get(), browser.get(), success.get(), offline.get(), trial.get(), expired.get(), details.get() };
    }

    ScreenView* viewFor(ActivationController::Screen s)
    {
        using S = ActivationController::Screen;
        switch (s)
        {
            case S::Welcome:     return welcome.get();
            case S::BrowserWait: return browser.get();
            case S::Success:     return success.get();
            case S::Offline:     return offline.get();
            case S::Trial:       return trial.get();
            case S::Expired:     return expired.get();
            case S::Details:     return details.get();
            case S::Error:       return welcome.get();
            case S::Loading:     default: return nullptr;
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        const auto screen = controller.screen();
        auto* next = viewFor(screen);
        if (next != active)
        {
            const int dir = directionFor(lastScreen, screen);

            // Finish any still-running previous transition cleanly.
            if (outgoing != nullptr && outgoing != active && outgoing != next)
            {
                outgoing->setVisible(false);
                outgoing->setTopLeftPosition(0, 0);
                outgoing->setAlpha(1.0f);
            }

            outgoing = active;
            active = next;
            if (active != nullptr)
            {
                active->refresh();
                active->setVisible(true);
                active->toFront(false);
            }
            layoutActive();
            startTransition(dir);
        }
        else if (active != nullptr)
        {
            active->refresh();
        }
        lastScreen = screen;

        if (controller.screen() == ActivationController::Screen::Success)
        {
            if (controller.config().reduceMotion || ! successAnim)
            {
                success->setPop(1.0f);
            }
            else
            {
                success->setPop(0.0f);
                successAnim->start();
            }
        }

        updateCloseButton();
        owner.repaint();
        if (owner.onActivationChanged)
            owner.onActivationChanged(controller.license().has_value());
    }

    //== Animation ============================================================
    void buildAnimators()
    {
        // runningInfinitely() makes progress climb past 1.0 without wrapping, so
        // wrap into [0,1) ourselves for a continuous loop.
        glowAnim.emplace(juce::ValueAnimatorBuilder{}
                             .withDurationMs(5000.0)
                             .runningInfinitely()
                             .withValueChangedCallback([this](float v)
                                                       {
                                                           glowPhase = (float) std::fmod(v, 1.0);
                                                           owner.repaint(glowRegion);
                                                       })
                             .build());

        // Directional slide + cross-fade between screens, clipped to screenHost.
        transitionAnim.emplace(juce::ValueAnimatorBuilder{}
                                   .withDurationMs(380.0)
                                   .withEasing(juce::Easings::createEaseInOutCubic())
                                   .withValueChangedCallback([this](float v) { applyTransition(v); })
                                   .withOnCompleteCallback([this] { finishTransition(); })
                                   .build());

        successAnim.emplace(juce::ValueAnimatorBuilder{}
                                .withDurationMs(520.0)
                                .withEasing(juce::Easings::createEaseOutBack())
                                .withValueChangedCallback([this](float v) { success->setPop(v); })
                                .build());

        // Modal appear/dismiss: fades + scales the whole overlay (scrim + panel).
        appearAnim.emplace(juce::ValueAnimatorBuilder{}
                               .withDurationMs(260.0)
                               .withEasing(juce::Easings::createEaseOut())
                               .withValueChangedCallback([this](float v)
                                                         {
                                                             appear_ = appearStart_
                                                                       + (appearTarget_ - appearStart_) * v;
                                                             applyAppear();
                                                         })
                               .withOnCompleteCallback([this]
                                                       {
                                                           appear_ = appearTarget_;
                                                           applyAppear();
                                                           if (appearTarget_ <= 0.0f)
                                                           {
                                                               owner.setVisible(false);
                                                               appear_ = 1.0f; // reset for next show
                                                               applyAppear();
                                                           }
                                                       })
                               .build());

        updater.addAnimator(*glowAnim);
        updater.addAnimator(*transitionAnim);
        updater.addAnimator(*successAnim);
        updater.addAnimator(*appearAnim);
        glowAnim->start();
    }

    void appear()
    {
        owner.setVisible(true);
        owner.toFront(true);
        if (controller.config().reduceMotion || ! appearAnim)
        {
            appear_ = 1.0f;
            applyAppear();
            return;
        }
        appear_ = 0.0f;
        applyAppear();
        appearStart_ = 0.0f;
        appearTarget_ = 1.0f;
        appearAnim->start();
    }

    void dismiss()
    {
        if (controller.config().reduceMotion || ! appearAnim)
        {
            owner.setVisible(false);
            appear_ = 1.0f;
            applyAppear();
            return;
        }
        appearStart_ = appear_;
        appearTarget_ = 0.0f;
        appearAnim->start();
    }

    // Combined panel scale: appear/dismiss animation * fit-to-window down-scale.
    [[nodiscard]] float panelScale() const { return appearScale_ * fitScale_; }

    void applyAppear()
    {
        owner.setAlpha(juce::jlimit(0.0f, 1.0f, appear_));
        appearScale_ = 0.94f + 0.06f * appear_;
        const float s = panelScale();
        const auto pivot = panelBounds.toFloat().getCentre();
        const auto transform = s >= 0.999f
            ? juce::AffineTransform()
            : juce::AffineTransform::scale(s, s, pivot.x, pivot.y);
        screenHost.setTransform(transform);
        if (closeButton != nullptr)
            closeButton->setTransform(transform);
        if (moonbaseBadge != nullptr)
            moonbaseBadge->setTransform(transform);
        owner.repaint();
    }

    // Screen "depth": entry screens are shallow, in-flow screens deeper. Moving
    // deeper slides the new view in from the right (forward); shallower slides
    // it in from the left (back).
    static int depth(ActivationController::Screen s)
    {
        using S = ActivationController::Screen;
        switch (s)
        {
            case S::Loading: case S::Welcome: case S::Error:                   return 0;
            case S::Offline: case S::BrowserWait:                              return 1;
            case S::Trial:   case S::Success: case S::Details: case S::Expired: return 2;
        }
        return 0;
    }

    static int directionFor(ActivationController::Screen from, ActivationController::Screen to)
    {
        return depth(to) >= depth(from) ? 1 : -1;
    }

    void startTransition(int dir)
    {
        slideDir = dir;
        const int w = screenHost.getWidth();

        if (active == nullptr)
        {
            if (outgoing != nullptr) { outgoing->setVisible(false); outgoing = nullptr; }
            return;
        }

        if (controller.config().reduceMotion || ! transitionAnim || w <= 0)
        {
            finishTransition();
            return;
        }

        active->setAlpha(0.0f);
        active->setTopLeftPosition(dir * w, 0); // start off-screen (right if forward)
        if (outgoing != nullptr)
        {
            outgoing->setAlpha(1.0f);
            outgoing->setTopLeftPosition(0, 0);
        }
        transitionAnim->start();
    }

    void applyTransition(float v)
    {
        const int w = screenHost.getWidth();
        if (active != nullptr)
        {
            active->setAlpha(v);
            active->setTopLeftPosition((int) std::lround((1.0f - v) * (float) slideDir * (float) w), 0);
        }
        if (outgoing != nullptr)
        {
            outgoing->setAlpha(1.0f - v);
            outgoing->setTopLeftPosition((int) std::lround(-v * (float) slideDir * (float) w), 0);
        }
    }

    void finishTransition()
    {
        if (active != nullptr)
        {
            active->setTopLeftPosition(0, 0);
            active->setAlpha(1.0f);
        }
        if (outgoing != nullptr)
        {
            outgoing->setVisible(false);
            outgoing->setTopLeftPosition(0, 0);
            outgoing->setAlpha(1.0f);
            outgoing = nullptr;
        }
    }

    //== Layout / paint =======================================================
    void layout()
    {
        auto b = owner.getLocalBounds();

        // Responsive: the panel scales with the window between sensible min/max
        // bounds, with margins + interior padding that scale too.
        constexpr int minPanelW = 320, minPanelH = 420, minMargin = 16;
        const int hMargin = juce::jlimit(16, 48, b.getWidth() / 12);
        const int vMargin = juce::jlimit(16, 48, b.getHeight() / 12);
        const int panelW = juce::jlimit(minPanelW, 600, b.getWidth() - hMargin * 2);
        const int panelH = juce::jlimit(minPanelH, 660, b.getHeight() - vMargin * 2);

        // When the host is smaller than the modal's minimum footprint, scale the
        // whole modal down to fit rather than clipping the panel.
        const int needW = panelW + minMargin * 2;
        const int needH = panelH + minMargin * 2;
        fitScale_ = juce::jmin(1.0f, (float) b.getWidth() / (float) needW,
                               (float) b.getHeight() / (float) needH);

        panelBounds = Rectangle<int>(0, 0, panelW, panelH)
                          .withCentre({ b.getCentreX(), b.getCentreY() });
        glowRegion = Rectangle<int>(panelBounds.getX(), panelBounds.getY() - 6, panelBounds.getWidth(), 14);

        const int padX = juce::jlimit(22, 44, panelW / 14);
        const int padY = juce::jlimit(20, 34, panelH / 16);
        auto content = panelBounds.reduced(padX, padY);
        if (controller.config().showMoonbaseBadge)
            content.removeFromBottom(44);
        contentArea = content;
        updateCloseButton();

        if (moonbaseBadge != nullptr)
        {
            moonbaseBadge->setVisible(controller.config().showMoonbaseBadge);
            moonbaseBadge->setBounds(panelBounds.getX(), panelBounds.getBottom() - 40,
                                     panelBounds.getWidth(), 40);
        }

        screenHost.setBounds(contentArea);
        layoutActive();
        applyAppear(); // re-applies the appear * fit scale for the new bounds
    }

    // The flow is dismissable only once a valid license is loaded — until then
    // the modal stays up to lock the host. So the close button appears only when
    // the host wired onClose AND there is a license.
    void updateCloseButton()
    {
        const bool canClose = (owner.onClose != nullptr) && controller.license().has_value();
        constexpr int closeSize = 28;
        if (closeButton != nullptr)
        {
            closeButton->setVisible(canClose);
            closeButton->setBounds(contentArea.getRight() - closeSize, contentArea.getY() + 6,
                                   closeSize, closeSize);
            closeButton->toFront(false);
        }
        const int inset = canClose ? (closeSize + 10) : 0;
        for (auto* v : views())
            v->headerRightInset = inset;
    }

    void layoutActive()
    {
        // Views fill the (clipping) host; transitions offset them horizontally.
        for (auto* v : views())
            v->setBounds(screenHost.getLocalBounds());
    }

    void paintChrome(Graphics& g)
    {
        auto b = owner.getLocalBounds().toFloat();
        if (controller.config().overlayBackdrop)
        {
            // Modal over a host (e.g. a plugin editor): dim what's behind so the
            // app shows through instead of an opaque takeover.
            g.fillAll(juce::Colours::black.withAlpha(0.58f));
        }
        else
        {
            juce::ColourGradient bg(lnf.palette.backgroundTop, b.getCentreX(), -b.getHeight() * 0.1f,
                                    lnf.palette.backgroundBottom, b.getCentreX(), b.getHeight(), true);
            bg.addColour(0.5, lnf.palette.backgroundMid);
            g.setGradientFill(bg);
            g.fillRect(b);
        }

        auto panel = panelBounds.toFloat();

        // Scale the panel by the appear/dismiss animation and the fit-to-window
        // down-scale (the scrim, drawn above, only fades via the component alpha).
        const float s = panelScale();
        juce::Graphics::ScopedSaveState saved(g);
        if (s < 0.999f)
            g.addTransform(juce::AffineTransform::scale(s, s, panel.getCentreX(), panel.getCentreY()));

        // soft outer shadow
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRoundedRectangle(panel.translated(0, 14).expanded(2.0f), 18.0f);

        juce::ColourGradient pg(lnf.palette.panelTop, panel.getX(), panel.getY(),
                                lnf.palette.panelBottom, panel.getRight(), panel.getBottom(), false);
        pg.addColour(0.58, lnf.palette.panelMid);
        g.setGradientFill(pg);
        g.fillRoundedRectangle(panel, 16.0f);
        g.setColour(lnf.palette.panelBorder);
        g.drawRoundedRectangle(panel, 16.0f, 1.0f);

        // signature top-edge glow (breathing)
        const float breathe = 0.85f + 0.15f * (0.5f + 0.5f * std::sin(glowPhase * kTwoPi));
        auto glowLine = Rectangle<float>(panel.getX() + panel.getWidth() * 0.18f, panel.getY() - 0.5f,
                                         panel.getWidth() * 0.64f, 2.0f);
        juce::ColourGradient gg(lnf.accent.withAlpha(0.0f), glowLine.getX(), 0,
                                lnf.accent.withAlpha(0.0f), glowLine.getRight(), 0, false);
        gg.addColour(0.5, Colour(0xff82cef1).withAlpha(breathe));
        g.setGradientFill(gg);
        g.fillRoundedRectangle(glowLine, 1.0f);
        // The "secured by moonbase" footer is the MoonbaseBadge child component.
    }

    ActivationComponent& owner;
    ActivationLookAndFeel lnf;
    ActivationController controller;

    juce::Component screenHost;

    std::unique_ptr<WelcomeView> welcome;
    std::unique_ptr<BrowserWaitView> browser;
    std::unique_ptr<SuccessView> success;
    std::unique_ptr<OfflineView> offline;
    std::unique_ptr<TrialView> trial;
    std::unique_ptr<ExpiredView> expired;
    std::unique_ptr<DetailsView> details;
    ScreenView* active = nullptr;
    ScreenView* outgoing = nullptr;
    ActivationController::Screen lastScreen = ActivationController::Screen::Loading;
    int slideDir = 1;

    Rectangle<int> panelBounds, contentArea, glowRegion;

    std::unique_ptr<CloseButton> closeButton;
    std::unique_ptr<MoonbaseBadge> moonbaseBadge;

    juce::AnimatorUpdater updater;
    std::optional<juce::Animator> glowAnim, transitionAnim, successAnim, appearAnim;
    float glowPhase = 0.0f;

    // Modal appear/dismiss animation state.
    float appear_ = 1.0f;       // 1 = fully shown, 0 = hidden
    float appearScale_ = 1.0f;  // panel scale derived from appear_
    float appearStart_ = 0.0f;
    float appearTarget_ = 1.0f;
    float fitScale_ = 1.0f;     // down-scale applied when the host is smaller than the modal minimum
};

//==============================================================================
ActivationComponent::ActivationComponent(ActivationConfig config)
    : impl(std::make_unique<Impl>(*this, std::move(config)))
{
    setSize(defaultWidth, defaultHeight);
}

ActivationComponent::~ActivationComponent() = default;

ActivationController& ActivationComponent::controller() { return impl->controller; }

void ActivationComponent::appear() { impl->appear(); }

void ActivationComponent::dismiss() { impl->dismiss(); }

void ActivationComponent::paint(Graphics& g) { impl->paintChrome(g); }

void ActivationComponent::resized() { impl->layout(); }

} // namespace moonbase::juce_integration
