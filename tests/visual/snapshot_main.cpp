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
        return compat::font(juce::Font::getDefaultMonospacedFontName(), height,
                            role == ActivationFonts::Role::heading);
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

// The browser link a previewed online activation waits on.
const juce::String kPreviewBrowserUrl = "https://demo.moonbase.sh/activate?token=preview";

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

// A trial on a product whose trials never expire: no exp, so no countdown.
moonbase::license makeEndlessTrial()
{
    auto lic = makeLicense(true);
    lic.expires_at.reset();
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

// A full license holding two add-ons here, for the license view's "Add-ons" row.
moonbase::license makeAddOnLicense()
{
    auto lic = makeLicense(false);
    lic.sub_products = {
        { "pack", true, false, false, std::nullopt, moonbase::sub_product_seats{ 3, 2, 1 } },
        { "keys", true, true, false, kPreviewNow + std::chrono::hours(24 * 5), std::nullopt },
    };
    return lic;
}

moonbase::sub_product_catalog_entry makeAddOn(const char* id, const char* name, const char* tagline)
{
    moonbase::sub_product_catalog_entry entry;
    entry.id = id;
    entry.product.id = std::string("demo-app.") + id;
    entry.product.name = name;
    entry.product.tagline = tagline;
    entry.included_in_license = true;
    return entry;
}

moonbase::sub_product_pricing makePrice(double amount, const char* cycle = nullptr)
{
    moonbase::sub_product_pricing pricing;
    pricing.id = cycle != nullptr ? "monthly" : "one-off";
    pricing.name = "Standard";
    pricing.price = { { "EUR", amount } };
    pricing.original_price = pricing.price;
    if (cycle != nullptr)
        pricing.recurrence = moonbase::sub_product_recurrence{ "Recurring", cycle };
    else
        pricing.recurrence = moonbase::sub_product_recurrence{ "OneOff", "" };
    return pricing;
}

// Every state an add-on row has: owned and active, a running trial, one to try,
// one deactivated here, one with every seat in use, a subscription for sale and
// an ended trial. Seven rows, so the list scrolls.
ActivationController::AddOnsInfo makeAddOns()
{
    using Status = moonbase::sub_product_trial_status;
    ActivationController::AddOnsInfo info;
    info.phase = ActivationController::AddOnsInfo::Phase::Ready;
    info.currency = "EUR";

    auto pack = makeAddOn("pack", "Laniakea Pack", "48 presets from the Laniakea sessions");
    pack.product.owned = true;
    pack.active = true;
    pack.seats = moonbase::sub_product_seats{ 3, 2, 1 };

    auto keys = makeAddOn("keys", "Keys", "Felt, electric and toy pianos");
    keys.active = true;
    keys.expires_at = kPreviewNow + std::chrono::hours(24 * 5);
    keys.trial = moonbase::sub_product_trial_offer{ Status::active, 14, keys.expires_at, false };
    keys.product.default_variation = makePrice(29);
    keys.buy_url = "https://demo.moonbase.sh/buy/demo-app.keys";

    auto strings = makeAddOn("strings", "Strings", "Bowed and plucked");
    strings.trial = moonbase::sub_product_trial_offer{ Status::available, 7, std::nullopt, false };
    strings.product.default_variation = makePrice(39);
    strings.buy_url = "https://demo.moonbase.sh/buy/demo-app.strings";

    auto brass = makeAddOn("brass", "Brass", "Section and solo brass");
    brass.product.owned = true;
    brass.revoked = true;
    brass.seats = moonbase::sub_product_seats{ 2, 1, 1 };

    auto mallets = makeAddOn("mallets", "Mallets", "Marimba, vibes and glockenspiel");
    mallets.product.owned = true;
    mallets.seats = moonbase::sub_product_seats{ 1, 1, 0 };

    auto choir = makeAddOn("choir", "Choir", "Twelve voices, four syllables");
    choir.product.default_variation = makePrice(4.99, "Monthly");
    choir.buy_url = "https://demo.moonbase.sh/buy/demo-app.choir";

    auto organ = makeAddOn("organ", "Organ", "Pipe and drawbar");
    organ.trial = moonbase::sub_product_trial_offer{ Status::expired, 7, kPreviewNow - std::chrono::hours(48), false };
    organ.product.default_variation = makePrice(19);
    organ.buy_url = "https://demo.moonbase.sh/buy/demo-app.organ";

    info.addOns = { pack, keys, strings, brass, mallets, choir, organ };
    return info;
}

// Three add-ons that fit without scrolling, the way the docs show the screen:
// one active, one in its trial, one to try or buy.
ActivationController::AddOnsInfo makeShortAddOns()
{
    auto info = makeAddOns();
    auto drumKit = makeAddOn("drum-kit", "Drum Kit", "Twelve kits, recorded dry and in the room");
    drumKit.trial = moonbase::sub_product_trial_offer{ moonbase::sub_product_trial_status::available, 7,
                                                       std::nullopt, false };
    drumKit.product.default_variation = makePrice(39);
    drumKit.buy_url = "https://demo.moonbase.sh/buy/demo-app.drum-kit";
    info.addOns = { info.addOns[0], info.addOns[1], drumKit };
    return info;
}

// A license bought with 12 months of updates that ended before 3.0.0 came out:
// it covers releases up to 2.3.0. The NotCovered snapshots run as 3.0.0.
moonbase::license makeUpdatesEndedLicense()
{
    auto lic = makeLicense(false);
    lic.licensed_product.current_release_version = "3.0.0";
    lic.release_access = moonbase::license_release_access{};
    lic.release_access->updates_until = kPreviewNow - std::chrono::hours(24 * 200);
    lic.release_access->up_to_version = "2.3.0";
    return lic;
}

// A license for every version up to 2.x.
moonbase::license makeUpToMajorLicense()
{
    auto lic = makeLicense(false);
    lic.licensed_product.current_release_version = "3.0.0";
    lic.release_access = moonbase::license_release_access{};
    lic.release_access->up_to_major = 2;
    return lic;
}

const juce::String kPreviewUpgradeUrl = "https://demo.moonbase.sh/buy/demo-app?variation=v1&license=license-7Q2X";

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

    // Opt-in check that the animation shim keeps advancing when ticked, set up
    // exactly like the panel glow: infinite, linear, wrapped into [0,1) by the
    // caller. Two seconds of a one-second loop has to yield values from both
    // cycles, which is what catches an easing that flattens past 1.0. Runs on
    // whichever backend the build picked: juce_animation when present, the
    // module's own implementation otherwise.
    if (argc > 1 && juce::String(argv[1]) == "--anim-check")
    {
        anim::Updater updater;
        float last = -1.0f;
        int changes = 0;
        auto animation = anim::AnimationBuilder{}
                             .withDurationMs(1000.0)
                             .runningInfinitely()
                             .withEasing(anim::easings::createLinear())
                             .withValueChangedCallback([&](float v)
                                                       {
                                                           const float wrapped = (float) std::fmod(v, 1.0);
                                                           if (std::abs(wrapped - last) > 0.0001f)
                                                               ++changes;
                                                           last = wrapped;
                                                       })
                             .build();
        updater.addAnimation(animation);
        animation.start();
        for (int i = 0; i <= 20; ++i)
            updater.update(static_cast<double>(i) * 100.0); // simulate 2s of vblanks

        std::cout << "anim-check: distinct values over 2s = " << changes
                  << " (expect ~20, i.e. it keeps looping)\n";
        return changes > 15 ? 0 : 1;
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

    // The welcome view's error slot holds two lines and truncates the rest. 01c is
    // the longest copy the controller puts there; 01d is a rejected activation,
    // the one message that used to quote the SDK's own, unbounded reason.
    writeSnapshot(outDir, "01c-welcome-error-long", [](ActivationController& c)
                  { c.setPreviewState(Screen::Error, {},
                                      "Activation isn't available for this product right now. "
                                      "If this keeps happening, contact the developer."); });

    writeSnapshot(outDir, "01d-welcome-error-rejected", [](ActivationController& c)
                  { c.setPreviewState(Screen::Error, {},
                                      "Activation was rejected. If this keeps happening, contact the developer."); });

    // Locked after having a license: the welcome copy says why, one per reason.
    writeSnapshot(outDir, "01e-welcome-deactivated", [](ActivationController& c)
                  { c.setPreviewState(Screen::Welcome, {}, {}, false, LockReason::Deactivated); });

    writeSnapshot(outDir, "01f-welcome-license-expired", [](ActivationController& c)
                  { c.setPreviewState(Screen::Welcome, {}, {}, false, LockReason::Expired); });

    writeSnapshot(outDir, "01g-welcome-license-invalid", [](ActivationController& c)
                  { c.setPreviewState(Screen::Welcome, {}, {}, false, LockReason::Invalid); });

    writeSnapshot(outDir, "01h-welcome-license-unverified", [](ActivationController& c)
                  { c.setPreviewState(Screen::Welcome, {}, {}, false, LockReason::Unverified); });

    writeSnapshot(outDir, "02-activating", [](ActivationController& c)
                  {
                      c.setPreviewState(Screen::BrowserWait);
                      c.setPreviewBrowserLink(kPreviewBrowserUrl);
                  });

    // The browser couldn't be opened (no default browser): copy the link instead.
    writeSnapshot(outDir, "02b-activating-no-browser", [](ActivationController& c)
                  {
                      c.setPreviewState(Screen::BrowserWait);
                      c.setPreviewBrowserLink(kPreviewBrowserUrl, /*opened*/ false);
                  });

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

    writeSnapshot(outDir, "06d-trial-endless", [](ActivationController& c)
                  { c.setPreviewState(Screen::Trial, makeEndlessTrial()); });

    writeSnapshot(outDir, "06e-trial-endless-ended", [](ActivationController& c)
                  { c.setPreviewState(Screen::Expired, makeEndlessTrial()); });

    {
        // A license that doesn't cover the version that runs.
        auto cfg = demoConfig();
        cfg.applicationVersion = "3.0.0";
        writeSnapshot(outDir, "06f-not-covered-updates-ended",
                      [](ActivationController& c)
                      { c.setPreviewNotCovered(makeUpdatesEndedLicense(), kPreviewUpgradeUrl); },
                      cfg);

        // Refused on this computer, so no upgrade link: the button starts an
        // online activation, which offers the upgrade.
        writeSnapshot(outDir, "06g-not-covered-up-to-major",
                      [](ActivationController& c) { c.setPreviewNotCovered(makeUpToMajorLicense()); },
                      cfg);

        writeSnapshot(outDir, "06h-not-covered-offline",
                      [](ActivationController& c)
                      {
                          auto lic = makeUpToMajorLicense();
                          lic.method = moonbase::activation_method::offline;
                          c.setPreviewNotCovered(lic);
                      },
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

        // 2.4.0 came out after this license's updates ended.
        writeSnapshot(outDir, "14b-update-not-covered",
                      [notes](ActivationController& c)
                      {
                          auto lic = makeUpdatesEndedLicense();
                          lic.licensed_product.current_release_version = "2.4.0";
                          lic.release_access->up_to_version = "2.3.1";
                          c.setPreviewUpdate(UpdatePhase::Ready, lic, notes, 0.0, {}, true,
                                             /*covered*/ false, kPreviewUpgradeUrl);
                      },
                      cfg);

        // A full license Moonbase keeps the release from, such as one limited
        // to a group of customers: nothing to unlock or buy.
        writeSnapshot(outDir, "14c-update-not-allowed",
                      [notes](ActivationController& c)
                      { c.setPreviewUpdate(UpdatePhase::Ready, makeUpdateLicense(), notes, 0.0,
                                           {}, /*canDownload*/ false); },
                      cfg);
    }

    //== Add-ons ==============================================================
    {
        auto cfg = demoConfig();
        cfg.showAddOns = true;

        // The license view's "Add-ons" row, under the card's other rows.
        writeSnapshot(outDir, "23-details-add-ons",
                      [](ActivationController& c) { c.setPreviewState(Screen::Details, makeAddOnLicense()); },
                      cfg);

        writeSnapshot(outDir, "23b-details-add-ons-error",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Details, makeAddOnLicense(),
                                          "Couldn't reach Moonbase to deactivate. Try again when online."); },
                      cfg);

        writeSnapshot(outDir, "24-add-ons",
                      [](ActivationController& c) { c.setPreviewAddOns(makeAddOnLicense(), makeAddOns()); },
                      cfg);

        writeSnapshot(outDir, "24-add-ons-short",
                      [](ActivationController& c) { c.setPreviewAddOns(makeAddOnLicense(), makeShortAddOns()); },
                      cfg);

        // Keys' trial ran out after the list loaded, which still calls it active:
        // the row goes by the license, which says it has ended.
        writeSnapshot(outDir, "24-add-ons-trial-ran-out",
                      [](ActivationController& c)
                      {
                          auto license = makeAddOnLicense();
                          license.sub_products[1].expires_at = kPreviewNow - std::chrono::hours(1);
                          auto info = makeShortAddOns();
                          info.addOns[1].expires_at = license.sub_products[1].expires_at;
                          info.addOns[1].trial->expires_at = license.sub_products[1].expires_at;
                          c.setPreviewAddOns(std::move(license), std::move(info));
                      },
                      cfg);

        // Rows that are easy to get wrong: an add-on bought during its trial (the
        // trial still unlocks it; a seat is the customer's to take), one whose only
        // seat is in use elsewhere, a trial behind a newsletter sign-up the customer
        // hasn't given, and a name too long for its row.
        writeSnapshot(outDir, "24-add-ons-edges",
                      [](ActivationController& c)
                      {
                          using Status = moonbase::sub_product_trial_status;
                          const auto trialEnds = kPreviewNow + std::chrono::hours(24 * 3);
                          auto license = makeAddOnLicense();
                          license.sub_products.push_back({ "tape-echo", true, true, false, trialEnds, std::nullopt });

                          ActivationController::AddOnsInfo info;
                          info.phase = ActivationController::AddOnsInfo::Phase::Ready;
                          info.currency = "EUR";

                          auto echo = makeAddOn("tape-echo", "Tape Echo", "Wow, flutter and a worn head");
                          echo.product.owned = true;
                          echo.active = true;
                          echo.expires_at = trialEnds;
                          echo.trial = moonbase::sub_product_trial_offer{ Status::active, 7, trialEnds, false };
                          echo.product.default_variation = makePrice(25);
                          echo.buy_url = "https://demo.moonbase.sh/buy/demo-app.tape-echo";

                          auto mallets = makeAddOn("mallets", "Mallets", "Marimba, vibes and glockenspiel");
                          mallets.product.owned = true;
                          mallets.seats = moonbase::sub_product_seats{ 1, 1, 0 };

                          auto choir = makeAddOn("choir", "Choir", "Twelve voices, four syllables");
                          choir.trial = moonbase::sub_product_trial_offer{ Status::unavailable, 7, std::nullopt, true };
                          choir.product.default_variation = makePrice(4.99, "Monthly");
                          choir.buy_url = "https://demo.moonbase.sh/buy/demo-app.choir";

                          auto expansion = makeAddOn("expansion", "Orchestral Percussion Expansion, Volume Two, With Every Extra",
                                                     "Timpani, bass drum, gongs and more");
                          expansion.trial = moonbase::sub_product_trial_offer{ Status::available, 14, std::nullopt, false };
                          expansion.product.default_variation = makePrice(149);
                          expansion.buy_url = "https://demo.moonbase.sh/buy/demo-app.expansion";

                          info.addOns = { echo, mallets, choir, expansion };
                          c.setPreviewAddOns(std::move(license), std::move(info));
                      },
                      cfg);

        // Drum Kit's trial was started, and has run out, since a list that still
        // offers it was loaded (reloading it failed): the license says the trial
        // is used up, so it isn't offered again.
        writeSnapshot(outDir, "24-add-ons-trial-used-up",
                      [](ActivationController& c)
                      {
                          auto license = makeAddOnLicense();
                          license.sub_products.push_back(
                              { "drum-kit", true, true, false, kPreviewNow - std::chrono::hours(2), std::nullopt });
                          auto info = makeShortAddOns(); // still offers Drum Kit's trial
                          info.error = "Couldn't reach Moonbase. Check your internet connection and try again.";
                          c.setPreviewAddOns(std::move(license), std::move(info));
                      },
                      cfg);

        writeSnapshot(outDir, "24b-add-ons-busy",
                      [](ActivationController& c)
                      {
                          auto info = makeAddOns();
                          info.busyId = "strings";
                          c.setPreviewAddOns(makeAddOnLicense(), std::move(info));
                      },
                      cfg);

        writeSnapshot(outDir, "24c-add-ons-no-seat",
                      [](ActivationController& c)
                      {
                          auto info = makeAddOns();
                          info.actionError = "No seat of Brass is free for this computer. "
                                             "Free one in your Helio Audio account, or buy one.";
                          c.setPreviewAddOns(makeAddOnLicense(), std::move(info));
                      },
                      cfg);

        writeSnapshot(outDir, "24d-add-ons-loading",
                      [](ActivationController& c)
                      {
                          ActivationController::AddOnsInfo info;
                          info.phase = ActivationController::AddOnsInfo::Phase::Loading;
                          c.setPreviewAddOns(makeAddOnLicense(), std::move(info));
                      },
                      cfg);

        writeSnapshot(outDir, "24e-add-ons-failed",
                      [](ActivationController& c)
                      {
                          ActivationController::AddOnsInfo info;
                          info.phase = ActivationController::AddOnsInfo::Phase::Ready;
                          info.error = "Couldn't reach Moonbase. Check your internet connection and try again.";
                          c.setPreviewAddOns(makeAddOnLicense(), std::move(info));
                      },
                      cfg);

        writeSnapshot(outDir, "24f-add-ons-empty",
                      [](ActivationController& c)
                      {
                          ActivationController::AddOnsInfo info;
                          info.phase = ActivationController::AddOnsInfo::Phase::Ready;
                          info.currency = "EUR";
                          c.setPreviewAddOns(makeAddOnLicense(), std::move(info));
                      },
                      cfg);

        auto daylightAddOns = daylightTheme();
        daylightAddOns.showAddOns = true;
        writeSnapshot(outDir, "24g-theme-daylight-add-ons",
                      [](ActivationController& c) { c.setPreviewAddOns(makeAddOnLicense(), makeAddOns()); },
                      daylightAddOns);

        auto emberAddOns = emberTheme();
        emberAddOns.showAddOns = true;
        writeSnapshot(outDir, "24h-theme-ember-add-ons",
                      [](ActivationController& c) { c.setPreviewAddOns(makeAddOnLicense(), makeAddOns()); },
                      emberAddOns);
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

        // Monospaced, so the widest a message gets.
        writeSnapshot(outDir, "15b-theme-ember-welcome-error",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Error, {},
                                          "Activation isn't available for this product right now. "
                                          "If this keeps happening, contact the developer."); },
                      emberTheme());

        // The longest lock-reason copy, monospaced.
        writeSnapshot(outDir, "15c-theme-ember-welcome-license-invalid",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Welcome, {}, {}, false, LockReason::Invalid); },
                      emberTheme());

        // Monospaced, so the no-browser copy runs widest here.
        writeSnapshot(outDir, "16-theme-ember-activating",
                      [](ActivationController& c)
                      {
                          c.setPreviewState(Screen::BrowserWait);
                          c.setPreviewBrowserLink(kPreviewBrowserUrl, /*opened*/ false);
                      },
                      emberTheme());

        writeSnapshot(outDir, "17-theme-ember-trial",
                      [](ActivationController& c)
                      { c.setPreviewState(Screen::Trial, makeLicense(true)); },
                      emberTheme());

        // Monospaced, so the NotCovered copy runs widest here.
        auto notCoveredCfg = emberTheme();
        notCoveredCfg.applicationVersion = "3.0.0";
        writeSnapshot(outDir, "17b-theme-ember-not-covered",
                      [](ActivationController& c)
                      {
                          auto lic = makeUpdatesEndedLicense();
                          lic.release_access->up_to_major = 2;
                          c.setPreviewNotCovered(lic, kPreviewUpgradeUrl);
                      },
                      notCoveredCfg);

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
