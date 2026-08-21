// Offscreen UI snapshot harness for visual regression testing.
//
// Renders the activation UI in every state to PNG files (no window, no network)
// via juce::Component::createComponentSnapshot, using the module's reduceMotion
// + setPreviewState seams for deterministic frames. The output directory is
// argv[1] (default: ./ui-snapshots). scripts/visual-snapshots.sh builds + runs
// this and uploads the folder to Argos.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>

#include <moonbase_licensing/moonbase_licensing.h>

using namespace moonbase::juce_integration;
using Screen = ActivationController::Screen;
using UpdatePhase = ActivationController::UpdateInfo::Phase;

namespace {

juce::File snapshotScratchDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("moonbase-ui-snapshots");
    dir.createDirectory();
    return dir;
}

ActivationConfig demoConfig()
{
    ActivationConfig config;
    config.endpoint = "https://demo.moonbase.sh";
    config.productId = "demo-app";
    // The public demo key. Parsed at construction but never used to verify in
    // preview mode (setPreviewState injects synthetic licenses directly).
    config.publicKey = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAutOqeUiPMgYjAwQ53CyKhJSqojr2bejce0CshQi9Hd8mNZbkoROx
oS56eIzehFSlX4YwHnF47AR1+fPOe7Q33Cgzd6d9xqksiMH7sWK2mADIlB66vZdW
uk3Me0UMB22Biy1RQbSRMivu79MxCofsympoL/5CFjJLd1u37kxjuRWVLjJS84Rr
3L2W7R7Exnno/giC+L/Dv711mjgstmtlAQm5ZINvFvoLA1eFTDs6nlCs3dpJSiq3
fsBUMT9FtudzS5As54jeT/8MB66fJJ0A1LQ/v5CW8ACQYseFSIoOKErD3xU7QLIJ
ERUn++6CVMPvZo67jVbTY+GCXYfW4gGVZQIDAQAB
-----END RSA PUBLIC KEY-----)";
    config.productName = "Solstice";
    config.manufacturerName = "Helio Audio";
    config.deviceName = "Studio Mac"; // fixed so the device chip doesn't render the build host's name
    config.accent = juce::Colour(0xff186cdc);
    config.trialLengthDays = 14;
    config.trialFeatures = {
        { "Full DSP engine, unrestricted", true },
        { "First 12 of 48 factory presets", true },
        { "All 48 presets & expansion packs", false },
        { "Use in commercial releases", false },
    };
    config.reduceMotion = true;

    // A unique, absent license file so the controller's startup load finds nothing.
    auto licenseFile = snapshotScratchDir().getChildFile("license.mb");
    licenseFile.deleteFile();
    config.licenseFile = licenseFile;
    return config;
}

//==============================================================================
// Themes.
//
// config.palette + config.fonts are the module's re-skin seam, so these render
// the same screens through deliberately unlike themes: a warm near-black with a
// monospaced face, a light one, and a deep green one. They are the regression
// net for the colour tokens. Anything still hardcoded in the UI shows up here as
// stock blue-grey chrome, a cyan glow or a white wash on a panel that has none.

// Warm near-black, amber accent, everything set in the monospaced face. This is
// the theme from issue #23: a plugin whose own UI is warm and monospaced.
ActivationConfig emberTheme()
{
    auto config = demoConfig();
    config.productName = "Ember";
    config.manufacturerName = "Foundry Audio";
    config.accent = juce::Colour(0xffe4a03c);

    auto& p = config.palette;
    p.backgroundTop = juce::Colour(0xff2a1c12);
    p.backgroundMid = juce::Colour(0xff150d08);
    p.backgroundBottom = juce::Colour(0xff0c0704);
    p.panelTop = juce::Colour(0xff1e1610);
    p.panelMid = juce::Colour(0xff150f0a);
    p.panelBottom = juce::Colour(0xff100b07);
    p.panelBorder = juce::Colour(0x18ffe9c9);
    p.hairline = juce::Colour(0x1affe9c9);
    p.panelShadow = juce::Colour(0x73140a02);
    p.overlayDim = juce::Colour(0x94140a02);

    p.cardFill = juce::Colour(0x08ffe9c9);
    p.trackFill = juce::Colour(0x14ffe9c9);
    p.skeleton = juce::Colour(0x12ffe9c9);
    p.scrollThumb = juce::Colour(0x80ffe9c9);
    p.scrollTrack = juce::Colour(0x1affe9c9);

    p.textPrimary = juce::Colour(0xfff7ecdc);
    p.textBody = juce::Colour(0xffe3d2ba);
    p.textBright = juce::Colour(0xffc8ae8d);
    p.textSecondary = juce::Colour(0xffa08a6e);
    p.textMuted = juce::Colour(0xff7a6752);

    p.ghostFill = juce::Colour(0x0affe9c9);
    p.ghostBorder = juce::Colour(0x24ffe9c9);
    p.ghostHover = juce::Colour(0x16ffe9c9);
    p.link = juce::Colour(0xffe4a03c);
    p.seatEmpty = juce::Colour(0x1affe9c9);
    p.onAccent = juce::Colour(0xff2a1a08);

    p.glow = juce::Colour(0xffffcf8a);
    p.spinnerTrack = juce::Colour(0x26ffe9c9);

    p.success = juce::Colour(0xff8fc46a);
    p.successFill = juce::Colour(0x248fc46a);
    p.successBorder = juce::Colour(0x598fc46a);
    p.trial = juce::Colour(0xffe4a03c);
    p.trialBright = juce::Colour(0xfff7cf82);
    p.onTrial = juce::Colour(0xff2a1a08);
    p.error = juce::Colour(0xffe8967a);
    p.errorStrong = juce::Colour(0xffd2603c);
    p.errorDeep = juce::Colour(0xffa8482c);
    p.dangerFill = juce::Colour(0x14d2603c);
    p.dangerBorder = juce::Colour(0x4cd2603c);

    // The typeface seam. A real plugin points these at bundled faces via
    // juce::Typeface::createSystemTypefaceFor; the snapshot uses the platform's
    // monospaced font so the render stays reproducible on any machine.
    config.fonts.makeFont = [](ActivationFonts::Role role, float height)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height,
                                            role == ActivationFonts::Role::heading ? juce::Font::bold
                                                                                   : juce::Font::plain));
    };
    return config;
}

// A light theme: the inversion every dark-on-light token has to survive.
ActivationConfig daylightTheme()
{
    auto config = demoConfig();
    config.productName = "Daylight";
    config.manufacturerName = "Northward Studio";
    config.accent = juce::Colour(0xff1b5fd0);

    auto& p = config.palette;
    p.backgroundTop = juce::Colour(0xfff4f6fa);
    p.backgroundMid = juce::Colour(0xffe9edf4);
    p.backgroundBottom = juce::Colour(0xffdfe4ec);
    p.panelTop = juce::Colour(0xffffffff);
    p.panelMid = juce::Colour(0xfffbfcfe);
    p.panelBottom = juce::Colour(0xfff4f7fb);
    p.panelBorder = juce::Colour(0x1e000f28);
    p.hairline = juce::Colour(0x14000f28);
    p.panelShadow = juce::Colour(0x2a0a1730);
    p.overlayDim = juce::Colour(0x5c0a1730);

    p.cardFill = juce::Colour(0x08000f28);
    p.trackFill = juce::Colour(0x14000f28);
    p.skeleton = juce::Colour(0x12000f28);
    p.scrollThumb = juce::Colour(0x66000f28);
    p.scrollTrack = juce::Colour(0x14000f28);

    p.textPrimary = juce::Colour(0xff11182a);
    p.textBody = juce::Colour(0xff2c3648);
    p.textBright = juce::Colour(0xff42506a);
    p.textSecondary = juce::Colour(0xff5c6b86);
    p.textMuted = juce::Colour(0xff8593a8);

    p.ghostFill = juce::Colour(0x08000f28);
    p.ghostBorder = juce::Colour(0x24000f28);
    p.ghostHover = juce::Colour(0x12000f28);
    p.link = juce::Colour(0xff1b5fd0);
    p.seatEmpty = juce::Colour(0x1a000f28);
    p.onAccent = juce::Colour(0xffffffff);

    p.glow = juce::Colour(0xff5aa0f0);
    p.spinnerTrack = juce::Colour(0x1e000f28);

    p.success = juce::Colour(0xff127a45);
    p.successFill = juce::Colour(0x1e127a45);
    p.successBorder = juce::Colour(0x59127a45);
    p.trial = juce::Colour(0xffb07908);
    p.trialBright = juce::Colour(0xffd9a33a);
    p.onTrial = juce::Colour(0xfffdf6e6);
    p.error = juce::Colour(0xffb03030);
    p.errorStrong = juce::Colour(0xffc23a3a);
    p.errorDeep = juce::Colour(0xff8f2626);
    p.dangerFill = juce::Colour(0x14c23a3a);
    p.dangerBorder = juce::Colour(0x4cc23a3a);
    return config;
}

// Deep green with a lime accent: a hue nowhere near the built-in blue, so a
// leftover accent-adjacent literal stands out immediately.
ActivationConfig forestTheme()
{
    auto config = demoConfig();
    config.productName = "Understory";
    config.manufacturerName = "Fernwood Audio";
    config.accent = juce::Colour(0xff5ed17c);

    auto& p = config.palette;
    p.backgroundTop = juce::Colour(0xff0e2018);
    p.backgroundMid = juce::Colour(0xff08130e);
    p.backgroundBottom = juce::Colour(0xff050c08);
    p.panelTop = juce::Colour(0xff0c1a13);
    p.panelMid = juce::Colour(0xff08120d);
    p.panelBottom = juce::Colour(0xff060e0a);
    p.panelBorder = juce::Colour(0x18d8ffe8);
    p.hairline = juce::Colour(0x1ad8ffe8);
    p.panelShadow = juce::Colour(0x73010703);
    p.overlayDim = juce::Colour(0x94010703);

    p.cardFill = juce::Colour(0x08d8ffe8);
    p.trackFill = juce::Colour(0x14d8ffe8);
    p.skeleton = juce::Colour(0x12d8ffe8);
    p.scrollThumb = juce::Colour(0x80d8ffe8);
    p.scrollTrack = juce::Colour(0x1ad8ffe8);

    p.textPrimary = juce::Colour(0xffeafff2);
    p.textBody = juce::Colour(0xffc4dfd0);
    p.textBright = juce::Colour(0xff9dc4ae);
    p.textSecondary = juce::Colour(0xff76a189);
    p.textMuted = juce::Colour(0xff58806a);

    p.ghostFill = juce::Colour(0x0ad8ffe8);
    p.ghostBorder = juce::Colour(0x24d8ffe8);
    p.ghostHover = juce::Colour(0x16d8ffe8);
    p.link = juce::Colour(0xff7ee08a);
    p.seatEmpty = juce::Colour(0x1ad8ffe8);
    p.onAccent = juce::Colour(0xff062012);

    p.glow = juce::Colour(0xffa8f5b8);
    p.spinnerTrack = juce::Colour(0x26d8ffe8);

    p.success = juce::Colour(0xff5ed17c);
    p.successFill = juce::Colour(0x245ed17c);
    p.successBorder = juce::Colour(0x595ed17c);
    p.trial = juce::Colour(0xffd9c04a);
    p.trialBright = juce::Colour(0xfff2e08a);
    p.onTrial = juce::Colour(0xff10190a);
    p.error = juce::Colour(0xffe8a08a);
    p.errorStrong = juce::Colour(0xffd85a4a);
    p.errorDeep = juce::Colour(0xffa8402e);
    p.dangerFill = juce::Colour(0x14d85a4a);
    p.dangerBorder = juce::Colour(0x4cd85a4a);
    return config;
}

// A fixed wall-clock anchor for every rendered date and trial countdown, so the
// snapshots are identical run to run regardless of the real date. Paired with
// ActivationController::setPreviewClock so trialDaysRemaining() measures against
// the same instant the license timestamps are built from. 2026-01-01 12:00:00 UTC.
const std::chrono::system_clock::time_point kPreviewNow{ std::chrono::seconds(1767268800) };

moonbase::license makeLicense(bool trial)
{
    moonbase::license lic;
    lic.id = "license-7Q2X";
    lic.activation_id = "activation-7Q2X";
    lic.trial = trial;
    lic.method = moonbase::activation_method::online;
    lic.licensed_product.id = "demo-app";
    lic.licensed_product.name = "Solstice";
    lic.licensed_product.current_release_version = "3.1.0";
    lic.issued_to.id = "user-1";
    lic.issued_to.name = "Alex Rivera";
    lic.issued_to.email = "alex@northward.studio";

    lic.issued_at = kPreviewNow;
    lic.validated_at = kPreviewNow;
    if (trial)
        lic.expires_at = kPreviewNow + std::chrono::hours(24 * 11); // 11 of 14 days left
    lic.seat_count = 3; // 2 of 3 device seats used
    lic.seats_used = 2;
    lic.token = "preview.preview.preview";
    return lic;
}

moonbase::license makeOfflineLicense()
{
    auto lic = makeLicense(false);
    lic.method = moonbase::activation_method::offline;
    lic.seat_count.reset(); // offline licenses are permanent: no seats
    lic.seats_used.reset();
    return lic;
}

moonbase::license makeExpiredTrial()
{
    auto lic = makeLicense(true);
    lic.expires_at = kPreviewNow - std::chrono::hours(24 * 6); // ended 6 days ago
    lic.seat_count.reset();
    lic.seats_used.reset();
    return lic;
}

// A full (non-trial) license that renews — i.e. a subscription with an expiry
// date, so the details screen shows "Expires" with a real date rather than "Never".
moonbase::license makeSubscriptionLicense()
{
    auto lic = makeLicense(false);
    lic.expires_at = kPreviewNow + std::chrono::hours(24 * 27); // renews in ~27 days
    return lic;
}

// A full license whose product has a newer released version (2.4.0) than the
// app version the update snapshots run with (2.3.1), so the update screen shows.
moonbase::license makeUpdateLicense()
{
    auto lic = makeLicense(false);
    lic.licensed_product.current_release_version = "2.4.0";
    return lic;
}

// A trial with the same available update; used to render the download-gated state
// (a trial can't download installers from an owners-only product).
moonbase::license makeUpdateTrialLicense()
{
    auto lic = makeLicense(true);
    lic.licensed_product.current_release_version = "2.4.0";
    return lic;
}

int gSnapW = ActivationComponent::defaultWidth;
int gSnapH = ActivationComponent::defaultHeight;

void writeSnapshot(const juce::File& outDir, const juce::String& name,
                   std::function<void(ActivationController&)> setup,
                   ActivationConfig config = demoConfig())
{
    ActivationComponent component(std::move(config));
    component.onClose = [] {}; // enable the close button for the snapshots
    component.setSize(gSnapW, gSnapH);
    component.controller().setPreviewClock(kPreviewNow); // pin the trial countdown

    // setPreviewState notifies synchronously, so the screen is active immediately.
    setup(component.controller());

    auto image = component.createComponentSnapshot(component.getLocalBounds(), false, 2.0f);

    auto file = outDir.getChildFile(name + ".png");
    file.deleteFile();
    juce::FileOutputStream stream(file);
    if (stream.openedOk())
    {
        juce::PNGImageFormat png;
        png.writeImageToStream(image, stream);
    }
    juce::Logger::outputDebugString("wrote " + file.getFullPathName());
}

} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    // Opt-in live check of the HTTP transport against the demo endpoint.
    if (argc > 1 && juce::String(argv[1]) == "--net-check")
    {
        auto config = demoConfig();
        moonbase::licensing licensing(
            config.toLicensingOptions(),
            std::make_shared<moonbase::memory_license_store>(),
            // A fixed identity rather than the runner's: this check exercises the
            // HTTP transport only, and reading real hardware would make it fail on
            // any machine with no readable device identity.
            std::make_shared<moonbase::static_device_id_resolver>("snapshot-host", "snapshot-device-id"),
            std::make_shared<juce_http_transport>());
        try
        {
            const auto request = licensing.request_activation();
            std::cout << "net-check OK\n  id=" << request.id
                      << "\n  browser=" << request.browser_url << "\n";
            return 0;
        }
        catch (const std::exception& ex)
        {
            std::cout << "net-check FAILED: " << ex.what() << "\n";
            return 1;
        }
    }

    // Opt-in check that the JUCE 8 animator actually advances when ticked (the
    // same Animator the spinner uses, driven the way the VBlank updater drives it).
    if (argc > 1 && juce::String(argv[1]) == "--anim-check")
    {
        juce::AnimatorUpdater updater;
        float last = -1.0f;
        int changes = 0;
        auto anim = juce::ValueAnimatorBuilder{}
                        .withDurationMs(1000.0)
                        .runningInfinitely()
                        .withValueChangedCallback([&](float v)
                                                  {
                                                      const float wrapped = (float) std::fmod(v, 1.0);
                                                      if (std::abs(wrapped - last) > 0.0001f)
                                                          ++changes;
                                                      last = wrapped;
                                                  })
                        .build();
        updater.addAnimator(anim);
        anim.start();
        for (int i = 0; i <= 20; ++i)
            updater.update(static_cast<double>(i) * 100.0); // simulate 2s of vblanks

        std::cout << "anim-check: distinct values over 2s = " << changes
                  << " (expect many, i.e. it animates)\n";
        return changes > 5 ? 0 : 1;
    }

    if (auto* w = std::getenv("MB_SNAPSHOT_W")) gSnapW = juce::String(w).getIntValue();
    if (auto* h = std::getenv("MB_SNAPSHOT_H")) gSnapH = juce::String(h).getIntValue();

    const juce::String arg = argc > 1 ? juce::String(argv[1]) : juce::String("ui-snapshots");
    auto outDir = juce::File::isAbsolutePath(arg)
                      ? juce::File(arg)
                      : juce::File::getCurrentWorkingDirectory().getChildFile(arg);
    outDir.createDirectory();

    writeSnapshot(outDir, "01-welcome", [](ActivationController& c)
                  { c.setPreviewState(Screen::Welcome); });

    writeSnapshot(outDir, "01b-welcome-error", [](ActivationController& c)
                  { c.setPreviewState(Screen::Error, {},
                                      "Couldn't reach Moonbase to start activation. "
                                      "Check your connection and try again."); });

    writeSnapshot(outDir, "02-activating", [](ActivationController& c)
                  { c.setPreviewState(Screen::BrowserWait); });

    writeSnapshot(outDir, "03-success", [](ActivationController& c)
                  { c.setPreviewState(Screen::Success, makeLicense(false)); });

    writeSnapshot(outDir, "04-offline-empty", [](ActivationController& c)
                  { c.setPreviewState(Screen::Offline); });

    writeSnapshot(outDir, "05-offline-ready", [](ActivationController& c)
                  {
                      c.setOfflineResponse(juce::File::getSpecialLocation(juce::File::tempDirectory)
                                               .getChildFile("Solstice-license.mb"));
                      c.setPreviewState(Screen::Offline);
                  });

    writeSnapshot(outDir, "05b-offline-error", [](ActivationController& c)
                  {
                      c.setOfflineResponse(juce::File::getSpecialLocation(juce::File::tempDirectory)
                                               .getChildFile("Solstice-license.mb"));
                      c.setPreviewState(Screen::Offline, {},
                                        "That response file isn't valid for this device.");
                  });

    writeSnapshot(outDir, "06-trial", [](ActivationController& c)
                  { c.setPreviewState(Screen::Trial, makeLicense(true)); });

    writeSnapshot(outDir, "06b-trial-expired", [](ActivationController& c)
                  { c.setPreviewState(Screen::Expired, makeExpiredTrial()); });

    {
        // Many features -> the list overflows and the viewport scrolls.
        auto cfg = demoConfig();
        cfg.trialFeatures = {
            { "Full DSP engine, unrestricted", true },
            { "First 12 of 48 factory presets", true },
            { "All 48 presets & expansion packs", false },
            { "Use in commercial releases", false },
            { "Priority email support", false },
            { "Free major-version upgrades", false },
            { "Multi-seat studio license", false },
            { "Expansion pack: Laniakea", false },
        };
        writeSnapshot(outDir, "06c-trial-overflow",
                      [](ActivationController& c) { c.setPreviewState(Screen::Trial, makeLicense(true)); },
                      cfg);
    }

    writeSnapshot(outDir, "07-details", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeLicense(false)); });

    writeSnapshot(outDir, "07b-details-subscription", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeSubscriptionLicense()); });

    writeSnapshot(outDir, "07c-details-error", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeLicense(false),
                                      "Couldn't reach Moonbase to deactivate. Try again when online."); });

    writeSnapshot(outDir, "08-details-offline", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeOfflineLicense()); });

    writeSnapshot(outDir, "09-details-deactivating", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeLicense(false), {}, /*busy*/ true); });

    {
        // Details with the "Update available" badge: an update exists but was
        // dismissed, so we stay on Details and offer the badge to re-open it.
        auto cfg = demoConfig();
        cfg.applicationVersion = "2.3.1"; // older than the license's released version
        writeSnapshot(outDir, "07d-details-update-available",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Details, makeUpdateLicense()); },
                      cfg);
    }

    {
        // The update screen runs with an app version (2.3.1) older than the
        // license's released version (2.4.0); release notes are plain text.
        auto cfg = demoConfig();
        cfg.applicationVersion = "2.3.1";
        // Plain-text release notes, long enough to overflow the card so the
        // snapshot exercises the scrollable area + scrollbar.
        const juce::String notes =
            "Version 2.4.0\n"
            "\n"
            "New oversampling modes up to 16x for cleaner high-gain tones.\n"
            "8 new factory presets from the Laniakea sound pack.\n"
            "Reworked the modulation matrix with per-slot depth control.\n"
            "Fixes Apple Silicon AU validation under Logic 11.\n"
            "Fixes a rare crash when loading presets saved in the 1.x series.\n"
            "Lower CPU usage in the analyzer view.\n"
            "Retina-correct metering on mixed-DPI multi-monitor setups.\n"
            "New A/B compare with copy-from-B and snapshot slots.\n"
            "MIDI learn now supports relative encoders.\n"
            "Improved oversampling latency reporting to the host.\n"
            "Various accessibility and keyboard-navigation improvements.";

        writeSnapshot(outDir, "10-update-loading",
                      [](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Loading, makeUpdateLicense()); },
                      cfg);

        writeSnapshot(outDir, "11-update-ready",
                      [notes](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Ready, makeUpdateLicense(), notes); },
                      cfg);

        writeSnapshot(outDir, "12-update-downloading",
                      [notes](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Downloading, makeUpdateLicense(), notes, 0.42); },
                      cfg);

        writeSnapshot(outDir, "13-update-error",
                      [](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Ready, makeUpdateLicense(), {}, 0.0,
                                           "Couldn't load the release details. "
                                           "Check your connection and try again."); },
                      cfg);

        writeSnapshot(outDir, "14-update-gated",
                      [notes](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Ready, makeUpdateTrialLicense(), notes, 0.0,
                                           {}, /*canDownload*/ false); },
                      cfg);
    }

    //== Themed renders =======================================================
    // The re-skin seam (config.palette + config.fonts), rendered through three
    // unlike themes across the screens that between them touch every token: the
    // spinner and glow, the trial pill and its progress gradient, cards and seat
    // pips, the drop zone, the success card, the danger washes, and the update
    // screen's skeleton + scrollbar.
    {
        writeSnapshot(outDir, "15-theme-ember-welcome",
                      [](ActivationController& c) { c.setPreviewState(Screen::Welcome); },
                      emberTheme());

        writeSnapshot(outDir, "16-theme-ember-activating",
                      [](ActivationController& c) { c.setPreviewState(Screen::BrowserWait); },
                      emberTheme());

        writeSnapshot(outDir, "17-theme-ember-trial",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Trial, makeLicense(true)); },
                      emberTheme());

        writeSnapshot(outDir, "18-theme-daylight-success",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Success, makeLicense(false)); },
                      daylightTheme());

        writeSnapshot(outDir, "19-theme-daylight-details",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Details, makeLicense(false)); },
                      daylightTheme());

        writeSnapshot(outDir, "20-theme-daylight-offline",
                      [](ActivationController& c) { c.setPreviewState(Screen::Offline); },
                      daylightTheme());

        writeSnapshot(outDir, "21-theme-forest-expired",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Expired, makeExpiredTrial()); },
                      forestTheme());

        auto updateCfg = forestTheme();
        updateCfg.applicationVersion = "2.3.1"; // older than the license's released version
        // Long enough to overflow the card, so the themed scrollbar renders too.
        const juce::String themedNotes =
            "Version 2.4.0\n"
            "\n"
            "New oversampling modes up to 16x for cleaner high-gain tones.\n"
            "8 new factory presets from the Laniakea sound pack.\n"
            "Reworked the modulation matrix with per-slot depth control.\n"
            "Fixes Apple Silicon AU validation under Logic 11.\n"
            "Fixes a rare crash when loading presets saved in the 1.x series.\n"
            "Lower CPU usage in the analyzer view.\n"
            "Retina-correct metering on mixed-DPI multi-monitor setups.\n"
            "New A/B compare with copy-from-B and snapshot slots.\n"
            "MIDI learn now supports relative encoders.\n"
            "Improved oversampling latency reporting to the host.\n"
            "Various accessibility and keyboard-navigation improvements.";

        writeSnapshot(outDir, "22-theme-forest-update",
                      [themedNotes](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Ready, makeUpdateLicense(), themedNotes); },
                      updateCfg);
    }

    juce::Logger::outputDebugString("UI snapshots written to " + outDir.getFullPathName());
    return 0;
}
