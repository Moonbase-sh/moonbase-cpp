# moonbase_licensing — JUCE module

License activation for JUCE 8 apps and plugins, with a polished built-in UI, in one
drop-in [JUCE module](https://github.com/juce-framework/JUCE/blob/master/docs/JUCE%20Module%20Format.md).
Add the module, fill in three fields, show one component.

<p align="center">
  <img src="../../assets/moonbase-juce-welcome.png" width="66%"
       alt="Welcome screen: the product logo and name, an 'Activate Solstice' heading, the line 'Unlock the full plugin through your Helio Audio account.', an 'Activate online' button, a 'No internet? Activate offline' link, and a 'Licensing secured by moonbase' footer.">
</p>
<p align="center"><em>Where activation starts: one button for the browser flow, an offline fallback, and your branding.</em></p>

<p align="center">
  <img src="../../assets/moonbase-juce-trial.png" width="49%"
       alt="Trial screen: a free-trial panel with days remaining, a progress bar, and an Unlock full version button.">
  <img src="../../assets/moonbase-juce-license.png" width="49%"
       alt="License details screen: licensed-to name, email, plan, activation type, expiry, seat count, and a Deactivate this device button.">
</p>

## Features

- **Native integration.** Talks to the Moonbase licensing API directly: online
  (browser) activation, offline (machine-file) activation, validation with a grace
  period, and server-side deactivation. Not a `juce::OnlineUnlockStatus` wrapper. (If
  you want that bridge instead, see [`examples/juce/`](../../examples/juce/).)
- **Cross-SDK device identity.** Implements the
  [Moonbase device fingerprint spec](../../FINGERPRINT_SPEC.md) v2, so a license
  activated in a web or Electron app built on `@moonbase.sh/licensing` validates in
  your plugin, and the other way round. No subprocess is spawned and no root-only
  file is read, so it works inside a sandboxed host and gives the same id whether or
  not the process is elevated. iOS and Android have no identifier unrelated apps can
  read, so they get a *scoped* spec id (`mbd2s_`) that is stable within the
  platform's own scope but not cross-SDK. Every reader lives in the core SDK, which
  needs no framework, so a non-JUCE app on the same device computes the same id; see
  [`docs/juce-module.md`](../../docs/juce-module.md#ios-and-android-use-a-scoped-identity).
  **Changed in 4.0.0**: see [Upgrading from 3.x](#upgrading-from-3x) if you have
  already shipped.
- **Built-in UI.** A configurable, themeable `ActivationComponent` (and one-call
  `ActivationDialog`) covering every state — welcome, activating, success, offline,
  trial, trial expired, license details, and update-available — with JUCE 8 animated
  transitions and drag-and-drop for offline license files. Designed to sit as a modal
  over your plugin and lock it until activated.
- **In-app updates.** When the user's license entitles them to a newer release than the
  installed app, an "Update available" screen shows the release notes and downloads the
  new installer for their platform in-app, with progress. It appears automatically when
  the plugin opens, stays reachable from an "Update available" badge on the license
  screen, and "Skip this update" is remembered until a newer version ships. Opt out with
  `config.enableUpdatePrompt = false`.
- **Zero third-party dependencies.** HTTP over `juce::WebInputStream` (no CURL), JSON
  via a bundled `nlohmann/json`, and RS256 verification via OS-native crypto:
  Security.framework (macOS/iOS), CNG/bcrypt (Windows), system libcrypto (Linux).
  Nothing to `brew`/`vcpkg`/`apt` install.
- **Batteries included.** Persists the license to a per-user file, fails loud on
  misconfiguration, exposes a diagnostics sink for field debugging, and can attach
  JUCE system/host telemetry to requests. Brandable end to end.

Requires **JUCE 8** (8.0.4+) and C++17. Supports macOS, Windows, Linux, iOS and Android.

<p align="center">
  <img src="../../assets/moonbase-juce-update.png" width="66%"
       alt="Update available screen: an 'Update available' pill, a 'Solstice 1.0.0 is ready' heading, a 'What's new' changelog card, a Download button, and a 'Skip this update' link.">
</p>
<p align="center"><em>The in-app update notification: release notes and a one-click installer download.</em></p>

## Add it to your project

Add this repository (as a git submodule, say) and point your build at the module folder.

### CMake

```cmake
juce_add_module(path/to/moonbase-cpp/modules/moonbase_licensing)

target_link_libraries(MyPlugin PRIVATE moonbase_licensing)
target_compile_definitions(MyPlugin PRIVATE JUCE_USE_CURL=0)  # keep the zero-dep HTTP path
```

### Projucer

*Modules → Add a module → Add a module from a specified folder…* and select
`modules/moonbase_licensing`. The bundled SDK headers and `nlohmann/json` resolve from
the module's own search paths — nothing else to set up.

## Configure and use

Only three fields are required; in a plugin the product and manufacturer names default
from your `JucePlugin_Name` / `JucePlugin_Manufacturer` macros.

```cpp
#include <moonbase_licensing/moonbase_licensing.h>
using namespace moonbase::juce_integration;

ActivationConfig config;
config.endpoint  = "https://your-tenant.moonbase.sh";
config.productId = "your-product";
config.publicKey = embeddedPublicKeyPem;       // your product's RSA public key
// Optional: config.productName / manufacturerName / accent / logo / strings ...
```

Show it as a modal over your editor (locks until activated), or pop it from a menu item:

```cpp
// Embedded modal:
auto activation = std::make_unique<ActivationComponent>(config);
activation->onClose = [this] { /* dismiss the modal */ };
addAndMakeVisible(*activation);

// …or a standalone window:
ActivationDialog::show(config, [](bool wasActivated) { /* update UI */ });
```

Gate your DSP/features on the live license state:

```cpp
if (! activation->controller().license().has_value())
    buffer.clear();   // not activated
```

`controller().license()` is the full `moonbase::license` (`trial`, `expires_at`,
`issued_to.email`, seat counts, sub-product ownership, custom `properties`, …) for
richer gating, and `onActivationChanged` fires whenever it changes.

## Going further

- **Branding** — everything in `ActivationConfig` after the connection fields is UI:
  names, accent colour, logo `Drawable`, overridable copy (`config.strings`), trial
  length + feature list, the removable Moonbase badge, and the activation URL.
  Re-skin deeper via `ActivationLookAndFeel::palette`.
- **Refresh entitlements** — `controller().refreshLicense()` re-validates online so a
  freshly purchased sub-product/upgrade loads without a restart (async, silent, with an
  optional completion callback).
- **App updates.** Set `config.applicationVersion` (or rely on `JucePlugin_VersionString`)
  and the update screen surfaces whenever the user's license entitles them to a newer
  release. It pops automatically on open (`config.autoPresentUpdate`), downloads installers
  into `config.downloadDirectory` (defaults to Downloads), and is fully optional
  (`config.enableUpdatePrompt = false`).
- **Persistence** — the validated license is stored at
  `userApplicationDataDirectory/<manufacturer>/<product>/license.mb` by default
  (override with `config.licenseFile`).
- **Diagnostics** — `config.onDiagnostic` receives the underlying reason behind any
  friendly error (bad config, rejected token, unreachable server).
- **Telemetry** — `config.analytics.enabled = true` attaches JUCE system/host metadata
  (OS, CPU, DAW host, plugin format, …) to activation requests; add your own via
  `config.metadata` / `config.onCollectMetadata`.

See [`docs/juce-module.md`](../../docs/juce-module.md) for the full guide and
[`examples/juce-native/`](../../examples/juce-native/) for a runnable sample app.

## Upgrading from 3.x

The device id changed in 4.0.0: the module now computes the cross-SDK
[fingerprint spec](../../FINGERPRINT_SPEC.md) id instead of
`juce::SystemStats::getUniqueDeviceID()`. If your plugin already has activated
users, their licenses are bound to the old id and will fail to validate, locking
them out until they re-activate, which consumes a fresh activation seat and resets
any device-scoped trial.

**iOS and Android are affected too.** They move to a scoped spec id (`mbd2s_`, from
`identifierForVendor` and `ANDROID_ID`) rather than the raw `getUniqueDeviceID()`
string, so the same migrating resolver applies. See
[iOS and Android use a scoped identity](../../docs/juce-module.md#ios-and-android-use-a-scoped-identity).

One line avoids that:

```cpp
config.deviceIdResolver = std::make_shared<moonbase::migrating_device_id_resolver>(
    // The platform default, NOT moonbase_device_id_resolver directly: on iOS and
    // Android that has no identity to read and throws, and a migrating resolver
    // asks its current resolver for an id before consulting any historical one,
    // so hard-coding it locks mobile users out of validation *and* activation.
    ActivationConfig::defaultDeviceIdResolver(),                                     // binds
    std::make_shared<moonbase::juce_integration::legacy_juce_device_id_resolver>()); // still accepted
```

New activations bind the spec id, existing licenses keep working, and the fleet
migrates as devices naturally re-activate. Drop the wrapper in a later release to
finish the migration.

Because `getUniqueDeviceID()` is JUCE's own derivation rather than a published
format, the historical resolver only vouches for a binding if the plugin still
ships the JUCE version that created it, so do not combine this upgrade with a JUCE
major bump.

See [`docs/juce-module.md`](../../docs/juce-module.md#migrating-an-already-shipped-plugin)
for the details, and [Migrating from 3.x](../../README.md#migrating-from-3x) for the
other options.
