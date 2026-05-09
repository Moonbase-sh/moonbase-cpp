# JUCE Integration

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
public key and the current device fingerprint, and silently treats invalid or
expired tokens as "not unlocked".

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

To revoke locally: `unlockStatus.clearLicense();`

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

The SDK's standard build (`MOONBASE_BUILD_EXAMPLES=ON`) does not pull in
JUCE — only the small `examples/activation.cpp` is compiled. JUCE is only
fetched when you opt in with `-DMOONBASE_BUILD_JUCE_EXAMPLE=ON` (see
above), or when you integrate `MoonbaseJuceBridge.h` into your own
JUCE/Projucer/CMake project alongside `target_link_libraries(your_target
PRIVATE moonbase::licensing)`.
