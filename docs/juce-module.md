# JUCE module: `moonbase_licensing`

A drop-in [JUCE module](https://github.com/juce-framework/JUCE/blob/master/docs/JUCE%20Module%20Format.md)
that adds Moonbase license activation — with a polished built-in UI — to any JUCE 8
app or plugin. It integrates **natively** with the Moonbase licensing API; it does
not use `juce::OnlineUnlockStatus`.

> Prefer the `juce::OnlineUnlockStatus` bridge? That older, copy-paste reference
> still lives in [`examples/juce/`](../examples/juce/) and is unchanged. This module
> is the native alternative.

Lives at [`modules/moonbase_licensing/`](../modules/moonbase_licensing/). Requires
**JUCE 8** (8.0.4+) and **C++17**.

## Why it's drop-in

A normal JUCE module is self-contained and links only JUCE + system frameworks.
This one is too — it has **no third-party dependencies**:

| Concern | How |
| --- | --- |
| HTTP | `juce::WebInputStream` (`juce_http_transport`) — no CURL |
| JSON | bundled `nlohmann/json` single header (in the module's `vendor/`) |
| RS256 JWT verification | OS-native: **Security.framework** (macOS/iOS), **CNG/bcrypt** (Windows), system **libcrypto** (Linux) |

So a downstream project adds the module and builds — nothing to `brew` / `vcpkg` /
`apt` install. *(The only platform caveat: on Linux the module links the
always-present system `libcrypto`. macOS and Windows need nothing beyond the OS.)*

The module sets `MOONBASE_CRYPTO_NATIVE` and `MOONBASE_DISABLE_CURL_TRANSPORT`
itself; the core SDK keeps its OpenSSL + CURL defaults for non-JUCE consumers, so
nothing about the existing `find_package(moonbase_cpp)` path changes.

## Add it to your project

Add this repository as a git submodule, then point your build at the module folder.

**CMake**

```cmake
juce_add_module(external/moonbase-cpp/modules/moonbase_licensing)

target_link_libraries(MyPlugin PRIVATE moonbase_licensing)
target_compile_definitions(MyPlugin PRIVATE JUCE_USE_CURL=0)
```

**Projucer** — *Modules → Add a module from a specified folder…* → select
`modules/moonbase_licensing`. The bundled SDK headers and `nlohmann/json` resolve
from the module's own search paths.

## Configure + show it

```cpp
#include <moonbase_licensing/moonbase_licensing.h>
using namespace moonbase::juce_integration;

ActivationConfig config;
config.endpoint    = "https://your-tenant.moonbase.sh";
config.productId   = "your-product";
config.publicKey   = embeddedPublicKeyPem;
config.productName = "Your Plugin";
config.manufacturerName = "Your Manufacturer";
config.accent      = juce::Colour(0xff186cdc);
```

Embed in a plugin editor:

```cpp
activation = std::make_unique<ActivationComponent>(config);
addAndMakeVisible(*activation);
activation->onActivationChanged = [this](bool active) { /* enable/disable UI */ };
```

…or pop it as a standalone window:

```cpp
ActivationDialog::show(config, [](bool wasActivated) { /* … */ });
```

## The flow

`ActivationComponent` owns an `ActivationController` — a headless state machine over
`moonbase::licensing`. Network calls run on a controller-owned thread pool; all state
changes and repaints happen on the message thread, gated by a generation counter so a
slow request can never clobber a newer state. Destroying the controller cancels any
in-flight request and joins its workers, so nothing keeps running after teardown — you
can call `start()` straight from the editor constructor and destroy the editor at any
time (plugin scanning, pluginval, rapid open/close) without deferring or guarding it.
The screens:

- **Welcome** — Activate online (browser flow) or Activate offline.
- **Activating** — opens the browser and polls `get_requested_activation()`; the
  device chip shows the local fingerprint + platform; Cancel aborts.
- **Success** — animated confirmation with a mini license card.
- **Offline** — two-step machine-file flow: save the request (`generate_device_token`),
  then load the response file (`read_offline_license`, validated locally).
- **Trial** — days-left badge, progress bar, included/excluded feature list, and an
  Unlock action that routes into online activation.
- **License details** — issued-to / email / plan / activation / expiry, a seat counter,
  and a Deactivate action (server-side `revoke_activation`, with a local-forget fallback
  for offline or trial licenses).
- **Trial expired** — a locked "trial has ended" screen with Unlock / Activate offline.
- **Update available.** Shown when a validated license reports a released version
  (the `current_release_version` / `p:rel` claim) newer than the app version. Loads the
  release notes from the inventory endpoints and downloads the new installer for the
  current platform in-app, with progress. "Skip this update" dismisses it (recorded so
  the quiet startup path won't re-prompt for that version; a newer release still does).
  Controlled by `config.enableUpdatePrompt` / `config.applicationVersion` /
  `config.downloadDirectory` / `config.autoPresentUpdate`.

All transitions use JUCE 8's animation API (`juce::Animator` / `ValueAnimatorBuilder`
/ `Easings`, driven by a `VBlankAnimatorUpdater`): cross-fade + fade-up between
screens, the activating spinner, the success pop, and the breathing top-edge glow.

## Gating

For correctness in a plugin, give the **processor** the controller (so license state
survives the editor's lifetime) and have the **editor** share it — one license, no
hand-rolled re-sync:

```cpp
// In your AudioProcessor:
ActivationController activation { makeConfig() };   // persistent
// activation.start();  // load any stored license

// In createEditor(): share the processor's controller with the UI.
auto* editor = new ActivationComponent (processor.activation);   // non-owning overload
```

Gate the audio thread off the lock-free flag — no `ChangeListener` needed:

```cpp
void processBlock (juce::AudioBuffer<float>& buffer, ...) override
{
    if (! activation.licensedFlag().load())
        buffer.clear();
}
```

Or use `LicenseGate` for a click-free fade on activate/deactivate (you still own the
gating; the module never silences audio itself):

```cpp
LicenseGate gate;                                  // a member of your processor
void prepareToPlay (double sr, int) override { gate.prepare (sr); gate.reset (activation.licensedFlag().load()); }
void processBlock (juce::AudioBuffer<float>& b, ...) override
{
    gate.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples(),
                  activation.licensedFlag().load());
}
```

`controller().license()` is the full `moonbase::license` — `trial`, `expires_at`,
`issued_to.email`, `owned_sub_product_ids`, custom `properties`, etc. — for richer
gating decisions (read it on the message thread).

## Branding / theming

Everything in `ActivationConfig` after the connection fields is brand/UI: product +
manufacturer name, `accent` colour, the Moonbase co-brand badge (`showMoonbaseBadge`), the
`trialLengthDays` + `trialFeatures` list (shown on the Trial / Expired screens),
`enableOffline`, and the `activationUrl`.

For a deeper re-skin, `config.palette` holds every other colour in the UI as its own
token, and `config.fonts` takes your typefaces for the three font roles:

```cpp
config.accent = juce::Colour(0xffe4a03c);

config.palette.backgroundTop = juce::Colour(0xff2a1c12);   // see ActivationTheme.h
config.palette.panelTop      = juce::Colour(0xff1e1610);   // for the full token list
config.palette.textPrimary   = juce::Colour(0xfff7ecdc);
config.palette.onAccent      = juce::Colour(0xff2a1a08);   // text drawn on the accent

// Bundle faces with juce_add_binary_data and hand them over per role. Do this
// rather than setting a default LookAndFeel: inside a DAW that one is shared
// with the host and every other plugin in the process.
config.fonts.heading = juce::Typeface::createSystemTypefaceFor(
    BinaryData::InterBold_ttf, BinaryData::InterBold_ttfSize);
config.fonts.body = juce::Typeface::createSystemTypefaceFor(
    BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize);
config.fonts.mono = juce::Typeface::createSystemTypefaceFor(
    BinaryData::SpaceMono_ttf, BinaryData::SpaceMono_ttfSize);
```

Set both before you construct the `ActivationComponent` (or pass the config to
`ActivationDialog::show`, which also frames its window in `palette.backgroundBottom`).
The component reads the theme once, when it builds its LookAndFeel and pre-renders its
icons, so a palette changed afterwards would only reach half the screen.

Leave any token alone and it keeps the built-in design's value, so a single line is a
valid theme. `config.fonts.makeFont` is the escape hatch when one typeface per role is
not enough: set it and it decides every font the UI asks for, by role and height.

## Device identity

By default the module identifies the device with
`moonbase::moonbase_device_id_resolver`, which implements the cross-SDK
[device fingerprint spec](../FINGERPRINT_SPEC.md) (`moonbase:fingerprint:v2`). A
license activated in a web or Electron app built on `@moonbase.sh/licensing`
validates in your plugin, and the other way round. See
[Device fingerprint](../README.md#device-fingerprint) for the stability contract and
the Linux caveats.

It still shells out to nothing: IOKit on macOS, world-readable files on Linux and
the firmware table on Windows all work inside a sandboxed host, and none of them
depend on the process running elevated.

Supply your own with `config.deviceIdResolver`, and inspect what the default
resolved to with `controller().describeDevice()`, which returns the id, spec
version, platform tag and the *names* of the contributing parameters. That is safe
to put behind a "Copy diagnostics" button; parameter values are hardware serial
numbers and are never exposed.

### Migrating an already-shipped plugin

**This changed in 4.0.0.** Earlier versions used
`juce::SystemStats::getUniqueDeviceID()`, so every already-activated user's license
is bound to that id. Without action they are locked out at next launch and must
re-activate, which consumes a fresh activation seat and resets any device-scoped
trial. One line avoids it:

```cpp
config.deviceIdResolver = std::make_shared<moonbase::migrating_device_id_resolver>(
    // The platform default, NOT moonbase_device_id_resolver directly: on iOS and
    // Android that has no identity to read and throws, and a migrating resolver
    // asks its current resolver for an id before consulting any historical one,
    // so hard-coding it locks mobile users out of validation *and* activation.
    ActivationConfig::defaultDeviceIdResolver(),                                     // binds
    std::make_shared<moonbase::juce_integration::legacy_juce_device_id_resolver>()); // still accepted
```

New activations bind the spec id; existing licenses keep validating; the fleet
migrates as devices naturally re-activate, and you drop the wrapper in a later
release. Because `getUniqueDeviceID()` is JUCE's own derivation rather than a
published format, the historical resolver only vouches for a binding if the plugin
still ships the JUCE version that created it, so do not combine this upgrade with a
JUCE major bump. Full options in
[Migrating from 3.x](../README.md#migrating-from-3x).

### iOS and Android use a scoped identity

On iOS and Android the module emits a **scoped** spec id, stamped `mbd2s_`, built
from `identifierForVendor` and `ANDROID_ID` respectively. Both readers live in the
core SDK, not in this module: the fingerprint is framework-independent by design,
so a non-JUCE app computes the same id.

Neither platform exposes a device identifier that unrelated applications can read.
iOS has had no accessible hardware serial since iOS 7, and `identifierForVendor` is
scoped to the App Store vendor; Android's `Build.SERIAL` returns `unknown` without a
privileged permission from Android 10, and `ANDROID_ID` has been scoped to the app
signing key since Android 8. **Cross-SDK parity on mobile is therefore impossible by
construction.** Rather than hide that, the spec's
[scoped identity](../FINGERPRINT_SPEC.md#scoped-identity) rules stamp the limitation
into the id: it is stable for your device within the platform's scope, and must
never be correlated with an id from another scope, in either direction.

Note the scope is the platform's, not yours: one vendor's two differently-signed
Android apps do not share an id.

`config.allowDeviceNameFallback` is **forbidden** on both, not merely ignored. Since
iOS 17 `gethostname()` returns the literal `localhost` on every device and
`UIDevice.name` returns the model name; on Android the label is `Build.MODEL`. An
SDK that fell through to the fallback when the scoped identifier was momentarily
absent would hand its entire install base one device id, and a single activation
would unlock every device. The ladder is scoped identity, then insufficient
identity, and nothing else. Absence is transient, so retry later.

> The Android reader deliberately does **not** use
> `juce::SystemStats::getUniqueDeviceID()`. That reads the *static field*
> `Settings.Secure.ANDROID_ID`, which is the key name `"android_id"` rather than the
> device's value, so every JUCE Android app reports the same id. The spec's
> `^[0-9a-f]{1,16}$` rule makes that mistake mechanically impossible here, since
> `"android_id"` is not hex.

**Mobile needs the same migration as desktop.** `mbd2s_<hash>` is a different value
from the raw `getUniqueDeviceID()` string bound before 4.0.0, so pass
`legacy_juce_device_id_resolver` as a historical resolver there too.

### When a machine has no hardware identity

Elsewhere, activation fails with `moonbase::insufficient_device_identity_error`
rather than binding something weak, and the controller routes it to the `Error`
screen with a diagnostic explaining why. This is reachable on cloned VM images whose
SMBIOS carries an unset UUID and a blank baseboard serial, and on minimal containers.

Set `config.allowDeviceNameFallback = true` to accept a weaker id derived from the
computer name instead. Those ids are stamped `mbd2n_` so the server can tell them
apart from real hardware bindings. Weigh it against the `"iPhone"` problem above:
the fallback is only safe where computer names are actually distinct.

## Sample app

[`examples/juce-native/`](../examples/juce-native/) is a runnable standalone app that
drops the component against the public Moonbase demo tenant:

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_NATIVE_EXAMPLE=ON
cmake --build build --target MoonbaseActivationNative
```

It fetches JUCE 8 on first configure and adds the module with `juce_add_module` —
exactly how a downstream project consumes it.

## Tests

Two automated suites cover the module:

- **Behavioral** ([`tests/juce/`](../tests/juce/)) drives `ActivationController` against
  injected fake store / transport / fingerprint and asserts the state machine: startup
  routing (Welcome / Details / Trial), expired-offline-license removal, online revoke vs.
  local forget vs. unreachable, and the offline activation round-trip. Tokens are signed
  with OpenSSL and verified by the module's native crypto backend, so the suite also
  exercises cross-backend RS256 parity.
- **Visual** ([`tests/visual/`](../tests/visual/)) renders every screen offscreen to PNGs
  for Argos diffing.

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_TESTS=ON
cmake --build build --target MoonbaseJuceTests
ctest --test-dir build -R "Juce\." --output-on-failure
```

The controller is built for injection: alongside the production constructor there is an
`ActivationController(config, std::shared_ptr<moonbase::licensing>, deviceName)` overload
that takes ready-made dependencies, so tests run with no network and no real device id.

Both suites can be built under a sanitizer (the controller and the license store both
spawn threads, so this guards against races and use-after-free):

```bash
cmake -B build-tsan -DMOONBASE_BUILD_TESTS=ON -DMOONBASE_SANITIZER=thread
cmake -B build-asan -DMOONBASE_BUILD_TESTS=ON -DMOONBASE_SANITIZER=address,undefined
```

CI: `sanitizers.yml` runs the SDK suite under ASan/UBSan and TSan on Linux; `juce.yml`
builds the module and runs the behavioral suite on macOS.

## Diagnostics

The UI shows friendly, end-user-facing copy. To see the underlying reason behind a failure
(bad config, rejected token, unreachable server, persist failure), wire a diagnostic sink:

```cpp
config.onDiagnostic = [] (const juce::String& message) {
    juce::Logger::writeToLog ("[activation] " + message);
};
```

It is invoked on the message thread. A missing or malformed `endpoint` / `productId` /
`publicKey` does not throw out of construction; the component shows an Error state and the
reason is reported through this sink (and `DBG` in debug builds).

**Escape hatch:** on the license details screen, double-clicking the green "Active" pill
reveals the configured license folder (where `license.mb` and `license.state.json` live) in
the OS file browser. It's an undocumented gesture (no pointer cursor) for support /
debugging, e.g. to inspect or clear persisted state.

## Refreshing entitlements

After a user buys something mid-session (a sub-product, an upgrade), re-validate the
license online so the new entitlements load without a restart:

```cpp
activation->controller().refreshLicense (/*force*/ true, [] (bool refreshed) {
    if (refreshed) reloadFeatures(); // read controller().license() again
});
```

It runs async and silently (no screen change). On success the license is updated +
persisted and `onActivationChanged` fires; `controller().license()` then reflects the new
`owned_sub_product_ids`, `properties`, expiry, and seat counts. `force` bypasses the
SDK's `online_validation_min_interval` throttle (you want that right after a purchase);
pass `false` for a polite background re-check that respects it. A network failure is
non-fatal: the current license is kept and the reason goes to `onDiagnostic`. Offline
licenses are a no-op (they are permanent and not server-tracked).

### Cadence and timeouts

How often the app re-checks online, how long it tolerates being offline, and the request
timeouts are all configurable:

```cpp
config.onlineCheckInterval = std::chrono::hours (1);     // min spacing between online checks (default 5 min)
config.onlineGracePeriod   = std::chrono::hours (24 * 30); // max time offline before locking (default 7 days)
config.httpConnectTimeout  = std::chrono::seconds (5);
config.httpRequestTimeout  = std::chrono::seconds (15);
```

The SDK never polls on a timer; it validates on launch (`start()`) and whenever you call
`refreshLicense()`, throttled to no more than once per `onlineCheckInterval`. A license
stays usable offline until `onlineGracePeriod` elapses since its last successful online
validation.

## App updates

A validated license carries the product's current released version in its claims
(`license.licensed_product.current_release_version`, the JWT `p:rel` claim). On launch
and after every re-validation, the controller compares it (via `moonbase::update_available`,
a small SemVer compare in `moonbase/version.hpp`) against the running app version:

```cpp
config.applicationVersion = "2.3.1";   // or rely on JucePlugin_VersionString
config.enableUpdatePrompt = true;       // default; set false to never prompt
config.downloadDirectory  = {};         // default: the user's Downloads folder
```

When the released version is newer, the **Update available** screen shows instead of the
Details / Trial screen (it never interrupts the locked Expired screen). It then:

1. Fetches the release notes (plain text) from
   `GET /api/customer/inventory/products/{productId}?version={released}`.
2. On "Download", resolves an authenticated, short-lived installer URL from
   `GET /api/customer/inventory/products/{productId}/download/{platform}/latest?redirect=false`
   and downloads it into `config.downloadDirectory` with progress, then reveals it.

**Download gating.** The product response reports the release access-control level
(`downloadsNeedsUser` / `downloadsNeedsOwnership`). `moonbase::can_download(license, info)`
applies it: a full (non-trial) license owns the product and can download; a trial cannot
download from an owners-only product (the default). When the license isn't entitled, the
screen swaps the Download button for an **Unlock full version** CTA and a short note, so
the user never hits a 403 (`updateInfo().canDownload`).

Both inventory requests authenticate with the license token via the
`Authorization: LicenseToken <jwt>` scheme (`moonbase::inventory_client`). Observe
`controller().updateInfo()` for the phase, versions, notes, progress, and `canDownload`.

**When it appears.** With `config.autoPresentUpdate` (default on), the module presents the
update screen automatically when the plugin **opens** with an available, non-skipped update
(the controller routes there on startup / re-validation and the component appears the
overlay). Opening the license view explicitly (e.g. the host's "License" button) never
auto-shows it — closing the update overlay returns the resting screen to the license view,
so re-opening lands on the license screen. The license view also keeps a clickable
**"Update available"** badge next to the "Active" pill (whenever `updateAvailable()` is
true) that opens it on demand; `ActivationComponent::presentUpdateIfAvailable()` is the
explicit host hook.

**"Skip this update".** Dismissing records the version in the `ignoredUpdates` list (so it
won't auto-present again for that version; a newer release still does). Where it goes
depends on how the screen was entered (tracked via `updateCameFromLicenseView()`): from the
license-view badge → back to the license view; auto-presented on open → it just closes the
overlay (the host's `onClose`).

**What is and isn't cached between sessions.** The released *version* is part of the
license token, so it is read from the persisted `license.mb` on every launch (no network
needed) and refreshed on the next successful online validation. The release *notes* and
the installer URL are **not** cached: `fetchUpdateInfo()` re-fetches the notes each time
the screen is shown, and the presigned installer URL is resolved fresh on each "Download"
click (it expires after ~15 minutes). The skip list lives in a small JSON state file
beside the license (`<license>.state.json`); that file (`ActivationState`) is the general
place for client state that should survive restarts, so future cached/remembered values
live there too.

## Telemetry / analytics

Off by default. One flag attaches JUCE system + host metadata to every activation and
validation request (the same `juce.*` keys the reference bridge collects):

```cpp
config.analytics.enabled = true;          // OS, CPU, JUCE version, memory
config.analytics.includeHostInfo = true;  // DAW host + plugin format (VST3/AU/AAX/...)
config.analytics.includeLocaleInfo = true; // language / region (opt in)
```

Host/plugin fields are captured only when `juce_audio_processors` is part of the build
(gated on JUCE's `JUCE_MODULE_AVAILABLE_juce_audio_processors`), so they light up
automatically in a plugin and are skipped in a plain app. Add your own fields, or rewrite
the assembled map last:

```cpp
config.metadata["app.channel"] = "beta";  // sent as-is; wins on key collisions
config.onCollectMetadata = [] (std::map<std::string, std::string>& m) {
    m["cohort"] = abTestCohort();
};
```

The collected map flows into `moonbase::licensing_options::metadata` and is sent with the
SDK's requests. When you don't set `config.applicationVersion`, it auto-fills from
`JucePlugin_VersionString` in a plugin build (or the running app's version otherwise), so
telemetry reports a version without extra wiring.
