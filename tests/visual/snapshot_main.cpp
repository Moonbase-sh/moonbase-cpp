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
    config.accent = juce::Colour(0xff186cdc);
    config.enableTrial = false; // online activation only
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

    const auto now = std::chrono::system_clock::now();
    lic.issued_at = now;
    lic.validated_at = now;
    if (trial)
        lic.expires_at = now + std::chrono::hours(24 * 11); // 11 of 14 days left
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

int gSnapW = ActivationComponent::defaultWidth;
int gSnapH = ActivationComponent::defaultHeight;

void writeSnapshot(const juce::File& outDir, const juce::String& name, std::function<void(ActivationController&)> setup)
{
    ActivationComponent component(demoConfig());
    component.onClose = [] {}; // enable the close button for the snapshots
    component.setSize(gSnapW, gSnapH);

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
            std::make_shared<juce_fingerprint_provider>(),
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

    writeSnapshot(outDir, "06-trial", [](ActivationController& c)
                  { c.setPreviewState(Screen::Trial, makeLicense(true)); });

    writeSnapshot(outDir, "07-details", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeLicense(false)); });

    writeSnapshot(outDir, "08-details-offline", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeOfflineLicense()); });

    writeSnapshot(outDir, "09-details-deactivating", [](ActivationController& c)
                  { c.setPreviewState(Screen::Details, makeLicense(false), {}, /*busy*/ true); });

    juce::Logger::outputDebugString("UI snapshots written to " + outDir.getFullPathName());
    return 0;
}
