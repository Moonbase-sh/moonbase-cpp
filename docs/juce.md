# JUCE Integration

> **Looking for a drop-in module with a built-in UI?** See
> [`docs/juce-module.md`](juce-module.md) for the `moonbase_licensing` JUCE module —
> native API integration plus a polished, themeable activation UI, with zero
> third-party dependencies. This page documents the older `juce::OnlineUnlockStatus`
> **bridge** (reference code you copy in), which remains available and unchanged.

A drop-in bridge for using the Moonbase C++ activation SDK from a JUCE-based
plugin or application. The bridge lives under [`examples/juce/`](../examples/juce/)
as reference code — copy the files into your project, since JUCE is not a
build dependency of the SDK itself.

## What you get

[`examples/juce/MoonbaseJuceBridge.h`](../examples/juce/MoonbaseJuceBridge.h) is a
single header containing four pieces under the `moonbase::juce_bridge`
namespace:

- **`MoonbaseJuceFingerprintProvider`** — implements `moonbase::fingerprint_provider`
  on top of `juce::SystemStats::getUniqueDeviceID()` (JUCE 7+).
- **`applyJuceMetadata(options)`** — fills `licensing_options.metadata` (and
  `application_version`) from JUCE's system + host helpers.
- **`MoonbaseUnlockStatus`** — subclass of `juce::OnlineUnlockStatus` that
  drives unlock state from Moonbase's JWT/browser activation flow.

[`examples/juce/PluginActivationComponent.h`](../examples/juce/PluginActivationComponent.h)
is a small `juce::Component` showing the full activation flow, paired with
[`examples/juce/Main.cpp`](../examples/juce/Main.cpp) for a standalone
JUCEApplication shell. The endpoint, product ID, and public key in that
component are hardcoded to the public Moonbase demo so the example runs out
of the box — replace them with your own values for production use.

## Building the standalone example

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_EXAMPLE=ON
cmake --build build --target MoonbaseJuceExample
```

The flag is opt-in (default off). On first configure CMake fetches JUCE
(~80 MB), and the first build compiles JUCE from source — several minutes.
Subsequent rebuilds reuse the cache. The output bundle/binary lives under
`build/examples/juce/MoonbaseJuceExample_artefacts/`.

## Wiring it up

Required JUCE modules: `juce_core`, `juce_product_unlocking`, and
(recommended) `juce_audio_processors` for host metadata.

```cpp
#include "MoonbaseJuceBridge.h"

moonbase::licensing_options options;
options.endpoint = "https://your-tenant.moonbase.sh";
options.product_id = "your-product";
options.public_key = embeddedPublicKeyPem;

moonbase::juce_bridge::applyJuceMetadata(options);

auto store = std::make_shared<moonbase::file_license_store>(
    juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("YourApp/license.mb").getFullPathName().toStdString());

moonbase::juce_bridge::MoonbaseUnlockStatus unlockStatus(std::move(options), std::move(store));
unlockStatus.tryLoadStoredLicense();
```

Call `tryLoadStoredLicense()` once from your `AudioProcessor` constructor or
application startup. It validates the stored JWT against your configured
public key and the current device fingerprint, and then re-validates against
the Moonbase API subject to the cadence + grace period configured on
`licensing_options`. Invalid, expired, or unreachable-past-grace tokens are
silently treated as "not unlocked".

The two `licensing_options` knobs that govern the API call:

- `online_validation_min_interval` (default 5 minutes) — if the local token's
  `validated_at` is newer than this, the API call is skipped entirely. Keeps
  `tryLoadStoredLicense()` cheap to call on every plugin instantiation.
- `online_validation_grace_period` (default 7 days) — maximum age the local
  token may reach without a successful online check. Within grace, transient
  transport failures fall back to the cached local result; beyond grace, they
  cause `tryLoadStoredLicense()` to return `false`.

Definitive server rejections (`license_invalid_error`, `license_expired_error`)
always cause `tryLoadStoredLicense()` to return `false` regardless of grace.
Offline-activated tokens (`activation_method::offline`) are validated locally
even when calling `tryLoadStoredLicense()` — the bridge never contacts the API
for them.

The call is synchronous. Inside the throttle window it's a single timestamp
comparison and returns immediately, but the first call past the window blocks
on libcurl. **For real plugins, prefer the async variant below** — DAWs will
flag the plugin as unresponsive if `AudioProcessor`'s constructor blocks on a
network call.

### Async (recommended for plugins)

```cpp
unlockStatus.tryLoadStoredLicenseAsync(
    [this](auto result) {
        // Runs on the JUCE message thread once the online check resolves.
        // The bridge has already updated its unlock state — just refresh UI.
        repaintActivationLabel();
    });
```

Behaviour:

1. Loads the stored license and runs **local validation synchronously** on
   the calling thread. The plugin/app is "unlocked" immediately if the
   cached token is locally valid — no message-thread blocking.
2. For online-activated tokens, kicks off `validate_token_online` on a
   background thread.
3. The result is marshalled back to the message thread, applied to the
   bridge's unlock state, and delivered to your callback.
4. **Threading**: state mutation and the callback always run on the JUCE
   message thread, regardless of which thread invoked
   `tryLoadStoredLicenseAsync` — so it's safe to call from
   `AudioProcessor`'s constructor even when the host runs that off the
   message thread.
5. **Staleness**: every call captures a generation number. If the user calls
   `clearLicense()`, finishes a new activation via `pollPendingActivation()`,
   or kicks off another `tryLoadStoredLicenseAsync` while a request is in
   flight, the older continuation is dropped silently — both state mutation
   and callback. This prevents a slow online check from resurrecting a
   license the user just cleared, or clobbering a freshly activated one.
6. If the bridge is destroyed while the check is in flight (e.g. the plugin
   is closed mid-request), the callback is silently dropped — the bridge's
   `juce::WeakReference` invalidates the message-thread continuation.

The callback receives an `AsyncValidationResult` whose `outcome` member is one
of: `NoStoredLicense`, `LocalInvalid`, `OfflineToken`, `Refreshed`,
`LockedInvalid`, `LockedExpired`, `Unreachable`. The bridge has already
updated its unlock state by the time the callback fires — checking
`isMoonbaseUnlocked()` is usually all you need; the outcome is there if you
want to show a more specific UI message ("server unreachable" vs. "license
revoked").

Within the grace period a transport failure is reported as `Refreshed` with
the cached license — the call effectively succeeded, falling back to the
locally trusted copy. Beyond grace, a transport failure becomes `Unreachable`
and the bridge transitions to locked.

## Activation flow

```cpp
const auto url = unlockStatus.beginActivation();
url.launchInDefaultBrowser();

// Then, on a juce::Timer running on the message thread:
void timerCallback() override
{
    if (unlockStatus.pollPendingActivation())
    {
        stopTimer();
        // unlockStatus.isMoonbaseUnlocked() is now true.
    }
}
```

`pollPendingActivation()` is non-blocking. The poll cadence is up to you;
once a second is plenty for a UI-driven flow.

## Deactivating

Two paths, depending on whether you want to free the seat server-side:

- `revokeActivation()` / `revokeActivationAsync(callback)` — calls the
  Moonbase backend to release this device's activation and then clears local
  state. Use this for "Deactivate" / "Sign out" buttons in your UI so the
  user can re-activate elsewhere without burning a seat.
- `clearLicense()` — local-only forget. Use it for offline or trial licenses
  (which can't be revoked), or when you just want this device to stop
  recognising the license without telling the server.

`revokeActivationAsync` is recommended for UI: the network call runs on a
`juce::Thread` and the callback is delivered on the message thread, with the
same generation-gating as `tryLoadStoredLicenseAsync` (a slow revoke can't
clobber a freshly activated license).

The callback receives a `RevokeOutcome`:

| Outcome | Meaning |
| --- | --- |
| `Revoked` | Seat freed server-side (or token was already gone). Bridge is now locked. |
| `NoLicense` | No license loaded — nothing to do. |
| `NotRevokable` | Token is offline-activated or a trial. Bridge state unchanged. Call `clearLicense()` if you still want a local forget. |
| `Unreachable` | Transport failure. Bridge state unchanged so the user can retry. |

```cpp
unlockStatus.revokeActivationAsync(
    [this](auto outcome) {
        using O = moonbase::juce_bridge::MoonbaseUnlockStatus::RevokeOutcome;
        if (outcome == O::NotRevokable)
            unlockStatus.clearLicense();    // trial / offline → local-only fallback
        repaintActivationLabel();
    });
```

## Offline activation

For machines without internet access, the bridge wraps Moonbase's file-based
flow. Emit a device token ("machine file"), have the user exchange it for a
license token, then load the token back in:

```cpp
// Step 1: write the machine file for the user to upload.
const auto deviceToken = unlockStatus.deviceTokenContents();
file.replaceWithText(juce::String(deviceToken)); // e.g. "MyPlugin.dt"

// Step 2: load the license token the user downloaded from the portal.
using O = moonbase::juce_bridge::MoonbaseUnlockStatus::OfflineActivationOutcome;
if (unlockStatus.activateOffline(downloadedFile.loadFileAsString()) == O::Activated)
    repaintActivationLabel(); // unlockStatus.isMoonbaseUnlocked() is now true
else
    showError("That license token is not valid for this device.");
```

The user can exchange the device token for a license token through any of:

- **Moonbase's hosted portal** — `https://<tenant>.moonbase.sh/activate`.
- **The embedded storefront on your own site** — the
  [`activate_product`](https://moonbase.sh/docs/storefronts/embedded/#call-methods)
  intent (`Moonbase.activate_product()`) prompts for the `.dt` and returns the
  license token file.
- **Your own custom flow** — drive the exchange with the Moonbase
  [APIs and SDKs](https://moonbase.sh/docs/licensing/offline-activations/).

`activateOffline` validates the token **locally only** (signature, audience,
device fingerprint, expiry, and that it was issued via offline activation),
persists it to the store on success, and transitions the bridge to unlocked. It
does no network I/O, so it's safe to call synchronously on the message thread.
Offline-activated tokens are never re-validated online and can't be revoked —
use `clearLicense()` for a local-only forget.

[`PluginActivationComponent.h`](../examples/juce/PluginActivationComponent.h)
wires both steps to **Save machine file…** / **Load license token…** buttons
using `juce::FileChooser`.

## Gating features

```cpp
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
{
    if (! unlockStatus.isUnlocked())     // or isMoonbaseUnlocked() — both agree.
    {
        buffer.clear();          // or render a tone, or apply a gain ducker, etc.
        return;
    }

    // ...your real DSP...
}
```

`unlockStatus.moonbaseLicense()` exposes the validated `moonbase::license`
for richer queries (`trial`, `expires_at`, `issued_to.email`, sub-product
ownership, custom `properties`).

## Inherited `juce::OnlineUnlockStatus` accessors

`MoonbaseUnlockStatus` keeps the inherited JUCE state in sync with the
underlying Moonbase license, so existing plugin code that goes through
`juce::OnlineUnlockStatus` keeps working:

| Accessor | Behaviour |
| --- | --- |
| `isUnlocked()` | `true` whenever `isMoonbaseUnlocked()` is true. |
| `getExpiryTime()` | Mirrors the moonbase license's `expires_at`; zero `Time` when the license has no expiry. |
| `getUserEmail()` | The validated user's email; empty after `clearLicense()`. |
| `getProductID()` | The configured Moonbase `product_id`. |
| `getLocalMachineIDs()` | Single-entry list with the moonbase device fingerprint. |

How this works: `MoonbaseUnlockStatus` generates a process-local 512-bit RSA
keypair on construction (used only inside the bridge — it never leaves the
process), and after each state change synthesizes a JUCE-format keyfile
signed with that key and hands it to `applyKeyFile()`. That's the only
public path into JUCE's private `status` ValueTree, so we go through it.

## Fingerprinting

`MoonbaseJuceFingerprintProvider` delegates to
`juce::SystemStats::getUniqueDeviceID()`, which JUCE 7+ derives from stable
hardware identifiers and hashes for you. If you don't pass a custom provider,
`MoonbaseUnlockStatus` defaults to it.

If your codebase predates JUCE 7 or you want the SDK's native fingerprint
(SMBIOS on Windows, `IOPlatformUUID` on macOS, board/BIOS/CPU on Linux), pass
`std::make_shared<moonbase::default_fingerprint_provider>()` as the third
constructor argument instead.

Switching providers between releases changes the device ID Moonbase sees,
which invalidates existing activations. Pick one when you ship and stay with
it.

## Metadata helper

`applyJuceMetadata(options, opts)` merges keys into `options.metadata`
(without overwriting any keys you've already set) and, when `includeAppVersion`
is on and `application_version` is unset, fills it from
`JUCEApplicationBase::getApplicationVersion()`.

| Key | Source | Group |
| --- | --- | --- |
| `juce.version` | `SystemStats::getJUCEVersion()` | system |
| `juce.os` | `SystemStats::getOperatingSystemName()` | system |
| `juce.os.is64Bit` | `SystemStats::isOperatingSystem64Bit()` | system |
| `juce.cpu.model` | `SystemStats::getCpuModel()` | system |
| `juce.cpu.vendor` | `SystemStats::getCpuVendor()` | system |
| `juce.cpu.cores` | `SystemStats::getNumPhysicalCpus()` | system |
| `juce.cpu.threads` | `SystemStats::getNumCpus()` | system |
| `juce.memoryMb` | `SystemStats::getMemorySizeInMegabytes()` | system |
| `juce.host.description` | `PluginHostType::getHostDescription()` | host (needs `juce_audio_processors`) |
| `juce.host.format` | `PluginHostType::getPluginLoadedAs()` | host |
| `juce.locale.display` | `SystemStats::getDisplayLanguage()` | locale (opt in) |
| `juce.locale.user` | `SystemStats::getUserLanguage()` | locale (opt in) |
| `juce.locale.region` | `SystemStats::getUserRegion()` | locale (opt in) |
| `juce.app.name` | `JUCEApplicationBase::getApplicationName()` | app |

Toggle the groups via `MoonbaseJuceMetadataOptions`:

```cpp
moonbase::juce_bridge::MoonbaseJuceMetadataOptions metaOpts;
metaOpts.includeLocaleInfo = true;        // off by default for privacy
metaOpts.includeHostInfo = false;         // skip DAW info
moonbase::juce_bridge::applyJuceMetadata(options, metaOpts);

options.metadata["app.channel"] = "beta"; // your own keys still go through
```

The helper deliberately omits `SystemStats::getFullUserName()`,
`getLogonName()`, and `getComputerName()` — PII that doesn't belong in
activation metadata. The computer name is already covered by
`fingerprint_provider::device_name()`.

If your toolchain has `juce_audio_processors` linked but the auto-detect
doesn't pick it up, define `MOONBASE_JUCE_HAS_AUDIO_PROCESSORS=1` before
including the header.

## Migrating from a JUCE keyfile flow

If you previously shipped with `juce::OnlineUnlockStatus` + JUCE keyfiles,
there is no automatic migration — the formats are unrelated. The simplest
upgrade is to require users to re-activate once via the Moonbase browser
flow. If you want to be friendlier, read the old keyfile yourself in your
processor's startup, decide whether to honour it for a grace period, and
prompt for Moonbase activation in the background.

`MoonbaseUnlockStatus::saveState`/`getState` still hold an opaque
`juce::String` you can use to round-trip your own migration breadcrumbs
through the existing `PropertiesFile` you used for the JUCE flow.

## CMake note

The SDK's top-level example build (`MOONBASE_BUILD_EXAMPLES=ON`) does not pull
in JUCE — only the small `examples/activation.cpp` is compiled. JUCE is only
fetched when you opt in with `-DMOONBASE_BUILD_JUCE_EXAMPLE=ON` (see
above), or when you integrate `MoonbaseJuceBridge.h` into your own
JUCE/Projucer/CMake project alongside `target_link_libraries(your_target
PRIVATE moonbase::licensing)`.
