# Migrating from `moonbase_JUCEClient`

`moonbase_JUCEClient` is Moonbase's earlier JUCE module, the one set up with a
`moonbase_api_config.json`, a prebuild script and `MOONBASE_*` macros. This page moves a
plugin or app from it to the [`moonbase_licensing` module](juce-module.md) without your
users activating again, and without costing them a seat or a trial.

Nothing changes in Moonbase. The product, its ID, your account and its signing key all
stay as they are, and builds still on the old module keep working, so you can ship the
new build whenever it is ready.

One setting carries your users over, the license the old module kept:

```cpp
// licenseFileLocation in the old module's moonbase_api_config.json
config.juceClientLicenseFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                   .getChildFile ("Your Company/Your Plugin/license.mb");
```

The rest is moving the code over (step 3).

## What your users see

The two modules keep licenses in different formats: the old one in Moonbase's earlier
format, this one as a signed JWT. On the first launch of the new build, the module finds
the license the old one left and has Moonbase convert it. The user sees their license,
as before: no browser, no second seat, and a trial carries on with the days it had left.
An offline license converts too, and stays offline.

- **Conversion needs a connection, once.** Without one the plugin starts locked, says why
  through `config.onDiagnostic`, and tries again at the next launch.
- **It happens once.** Instances starting together convert the license once, and the
  module doesn't try again once Moonbase has refused it for good (it was revoked, say),
  or once the new build has removed a license of its own: a user who deactivates in the
  new build stays deactivated. A converted license that couldn't be saved is converted
  again at the next launch.
- **Only a license for this computer and this product converts.** Moonbase can't tell
  whether the id a license is bound to and this computer's are one machine; only the
  computer can. So the module converts a license only when it is bound to an id
  `config.deviceIdResolver` returns or accepts, which the default does for the id the old
  module computed here, and is for `config.productId`. Anything else is left alone, and
  the reason goes to `config.onDiagnostic`.

## 1. Point at the old license

**`juceClientLicenseFile`** is the license the old module kept: `licenseFileLocation` from
its `moonbase_api_config.json`, under the user's application data folder (`~/Library` on
macOS, `%APPDATA%` on Windows). An Intel build running under Rosetta kept its own license
beside it, as `<name>_Rosetta.mb`, and the module reads that one under Rosetta too, as
the old module did.

The converted license goes where this module keeps its own, `licenseFile`, by default
`<application data>/<manufacturer>/<product>/license.mb`. When you set the two modules up
with the same names, that is the old module's file, and the converted license replaces
it. That costs nothing extra: on a computer that converts, a build still on the old
module has lost its activation to the move anyway (see
[Running both side by side](#running-both-side-by-side)). Until a conversion succeeds,
the file stays as it is.

If you [keep the old id](#or-keep-the-old-id) and builds on both modules will run on the
same computers, give this module a file of its own instead, so neither replaces the
other's license:

```cpp
config.licenseFile = config.juceClientLicenseFile.getSiblingFile ("license2.mb");
```

When the module finds the old module's license in `licenseFile` without being told to
convert it, it leaves the file in place, starts with no license, and says so through
`config.onDiagnostic`.

## 2. The device id

Moonbase keys a license seat and a trial by the device id the app presents. The old
module derived its id from `juce::SystemStats::getUniqueDeviceID()`. This module
identifies a device by the cross-SDK [fingerprint](device-identity.md), the id every
Moonbase SDK computes, so a license activated in a web or Electron app built on
`@moonbase.sh/licensing` is the same device as your plugin.

With `juceClientLicenseFile` set and `config.deviceIdResolver` left alone, the module
binds the cross-SDK id and keeps accepting the old one. Conversion moves each online
activation, and each trial, to the new id, keeping the seat and the days left. An offline
license stays bound to the old id, which the module goes on accepting while
`juceClientLicenseFile` is set, so keep it set while such licenses are out there.

The move is one-way. On a computer that moved, a build still on the old module finds its
activation revoked at its next online check, and activating it again takes a second
seat. See [Running both side by side](#running-both-side-by-side).

- **A custom salt or id.** If you set `APIInitializer::customMachineHashSalt`, name the
  old id yourself, with the same salt:

  ```cpp
  config.deviceIdResolver = std::make_shared<moonbase::migrating_device_id_resolver>(
      ActivationConfig::defaultDeviceIdResolver(),                // binds
      std::make_shared<moonbase::juce_integration::juce_client_device_id_resolver> (
          "your salt"));                                          // still accepted
  ```

  If you set `APIInitializer::machineIdCallback`, the old module bound whatever it
  returned, so put a resolver returning the same value in its place (a
  `moonbase::static_device_id_resolver` does for a value you already have).
- **Stay on the same JUCE.** `getUniqueDeviceID()` is JUCE's own derivation, not a
  published format, so the old id is reproduced only on a JUCE that derives it the way the
  old build's JUCE did. Move modules first and upgrade JUCE in a later release. It needs
  JUCE 7.0.5 or later, which the old module required anyway.
- **Not on Android.** There, `getUniqueDeviceID()` reads a constant rather than the
  device's id, so the old module bound the same id on every Android device. The module
  doesn't accept it, so Android licenses don't convert: users activate again.

### Or keep the old id

To go on binding the id the old module bound, set the resolver to it:

```cpp
config.deviceIdResolver =
    std::make_shared<moonbase::juce_integration::juce_client_device_id_resolver>(); // or ("your salt")
```

Licenses then convert where they are, nothing moves, and a computer with both builds
installed holds one seat between them. Keep the old id while builds on both modules run on
the same computers for a while, or to keep rolling back free. The trade-off is that a web
or Electron app sees the same computer as a different device.

You can move later. Drop the resolver in a later release, with `juceClientLicenseFile`
still set: licenses bound to the old id keep validating, and new activations bind the
cross-SDK id.

## 3. Move the code

### Project setup

| `moonbase_JUCEClient` | `moonbase_licensing` |
| --- | --- |
| The `moonbase_JUCEClient` submodule, folder name fixed | This repository as a submodule, or FetchContent; see [Add it to your project](juce-module.md#add-it-to-your-project) |
| `PreBuild.sh` / `PreBuild.ps1`, the generated config header, the key integrity step | Nothing to generate: the configuration is plain C++ |
| `moonbase_api_config.json` as a binary resource or `MOONBASE_JUCECLIENT_CONFIG_JSON` | Gone; see [Configuration](#configuration) |
| `juce_product_unlocking`, `juce_cryptography`, `juce_audio_basics` | `juce_core`, `juce_events`, `juce_data_structures`, `juce_graphics`, `juce_gui_basics` |
| `INCLUDE_MOONBASE_UI`, `MOONBASE_APP_MODE`, `ENABLE_WEBUI_HELPERS` | Nothing to set |
| C++17 (C++20 for the web UI helpers), JUCE 7.0.6 or later | C++17, JUCE 6.1.3 or later (7.0.5 or later to convert licenses) |

On Linux, keep `JUCE_USE_CURL` on: see [Linux needs libcurl](juce-module.md#linux-needs-libcurl).

### Configuration

| `moonbase_api_config.json` / `APIInitializer` | `ActivationConfig` |
| --- | --- |
| `baseUrl` | `endpoint` |
| `productId` | `productId` |
| `publicKey` | `publicKey`, as PEM. It is the same key: copy the PEM from your product's C++ implementation guide in the Moonbase dashboard. Don't generate a new one, since every license your users hold is signed with it |
| `offlineUrl` | `activationUrl` (defaults to `endpoint` + `/activate`); an empty `offlineUrl` becomes `enableOffline = false` |
| `licenseFileLocation` | `juceClientLicenseFile` (see [step 1](#1-point-at-the-old-license)) |
| `companyName`, `productName`, `productVersion` | `manufacturerName`, `productName`, `applicationVersion` (in a plugin these default to `JucePlugin_Manufacturer`, `JucePlugin_Name` and `JucePlugin_VersionString`) |
| `customMachineHashSalt`, `machineIdCallback` | `deviceIdResolver` (see [step 2](#2-the-device-id)) |
| `setGracePeriodDays`, `setOnlineVerificationGracePeriodDays` | `onlineGracePeriod` |

The old module allowed 14 days without an online check and this one defaults to 7. Set
`config.onlineGracePeriod = std::chrono::hours (24 * 14);` to keep what your users had.

### Code

A plugin on the old module:

```cpp
// PluginProcessor.h
MOONBASE_DECLARE_LICENSING_USING_JUCE_PROJECTINFO

// prepareToPlay
MOONBASE_PREPARE_TO_PLAY (sampleRate, samplesPerBlock);

// processBlock, last
MOONBASE_PROCESS (buffer);

// PluginEditor.h
MOONBASE_DECLARE_AND_INIT_ACTIVATION_UI (audioProcessor);

// a "License" button, and resized()
MOONBASE_SHOW_ACTIVATION_UI;
MOONBASE_RESIZE_ACTIVATION_UI;
```

The same plugin on this module:

```cpp
// PluginProcessor.h
moonbase::juce_integration::ActivationController activation { makeActivationConfig() };
moonbase::juce_integration::LicenseGate gate;

// the processor's constructor
activation.start();

// prepareToPlay
gate.prepare (sampleRate);
gate.reset (activation.licensedFlag().load());

// processBlock
gate.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(),
              activation.licensedFlag().load());

// PluginEditor.h: shares the processor's controller
moonbase::juce_integration::ActivationComponent activationUi { audioProcessor.activation };

// the editor's constructor: hidden until needed, it presents itself while the plugin is locked
addChildComponent (activationUi);

// a "License" button, and resized()
activationUi.appear();
activationUi.setBounds (getLocalBounds());
```

The old module cut the audio for 3 seconds in every 10 while unlicensed. `LicenseGate`
mutes it, with a short fade; what happens to unlicensed audio is up to you, since the
module never touches it itself. See [Gating](juce-module.md#gating).

| `moonbase_JUCEClient` | `moonbase_licensing` |
| --- | --- |
| `isUnlocked()` | `licensedFlag()` (any thread, lock-free), or `license().has_value()` on the message thread |
| `isTrial()`, `isOfflineActivated()` | `license()->trial`, `license()->method == moonbase::activation_method::offline` |
| `getUserEmail()`, `getUserName()`, `getUserId()`, `getLicenseId()` | `license()->issued_to.email`, `.name`, `.id`, and `license()->id` |
| `getLicenseExpiration()` | `license()->expires_at` |
| `getOwnedSubProducts()` | `hasSubProduct()`, `activeSubProducts()`, `subProductFlag()` for the audio thread, and `license()->owned_sub_product_ids`; see [Add-ons](juce-module.md#add-ons) |
| `get{Product,Customer,License,Trial}Properties()` | `license()->properties` |
| `addActivationStateChangedCallback()` | `onLicenseChanged` |
| `ActivationUI::Listener::onActivationUiVisibilityChanged` | `ActivationComponent::onClose` and `onActivationChanged` |
| `requestAutoActivation()` and polling | `beginOnlineActivation()`; the built-in UI does it for you |
| `requestOnlineActivation (email, password)` | No equivalent: activation goes through the browser |
| `generateMachineFile()`, `loadOfflineLicenseFile()` | `saveOfflineRequest()`, `setOfflineResponse()` and `activateOffline()`, or the built-in Offline screen |
| `deactivateLicense()` | `deactivate()` |
| `revalidateLicense()` | `refreshLicense()` |
| `isUpdateAvailable()`, `enableUpdateBadge()` | `updateAvailable()`, and the built-in update screen; see [App updates](juce-module.md#app-updates) |
| `setTransmitAnalytics()`, `registerGetAnalyticsCallback()` | `config.analytics`, `config.metadata`, `config.onCollectMetadata` (off by default, where the old module was on); see [Telemetry](juce-module.md#telemetry--analytics) |
| `setWelcomePageText()`, `setCompanyLogo()`, `setSpinnerLogo()` | `productName`, `manufacturerName`, `logo`, `strings`, `accent`, `palette`; see [Branding](juce-module.md#branding--theming) |
| `WebBrowserOptions` (the web UI helpers) | No equivalent: expose the controller to your web view through native functions of your own |
| `MB_IS_UNLOCKED_OBFUSCATED` | No equivalent; see [Security](security.md) |

## Running both side by side

Moonbase still answers the old module in its own format, for activating, re-validating,
deactivating and offline activation, so builds on either module can sell, activate, trial
and offline-activate the same product at the same time. What differs is a computer that
has both installed:

- **By default**, the new build moves the device to the cross-SDK id as it converts, and
  the old build there finds its activation revoked at its next online check (and, where
  the two share a license file, its license replaced). Activating the old build again
  takes a second seat.
- **With the old id kept** and a license file each, the two hold one seat and one trial
  between them. Deactivating in either one frees that seat, and the other locks the next
  time it checks with Moonbase.

To roll back, ship the old build again. After the default move, each computer that
converted has to activate the old build again, which takes a seat until you revoke the
moved activation in the dashboard. With the old id kept and a license file each, the old
build carries on with the license and the seat it always had.

## Before you ship

Try the move on a test product, or on a copy of your own, with builds on both modules:

- [ ] Activate online in the old build, then open the new one. It shows the license
      without activating, and Moonbase shows the device's activation under its new id,
      with the license's seat count unchanged.
- [ ] Start a trial in the old build. The new build shows the same trial, with the same
      days left.
- [ ] Activate offline in the old build. The new build shows the offline license.
- [ ] Open the new build for the first time with no connection. It starts locked, and the
      old license file is unchanged; open it again once connected, and it converts.
- [ ] Deactivate in the new build and open it again: it stays deactivated.
- [ ] With both installed, check what the old build does once the new one has converted:
      it reports its activation revoked by default, and keeps working with the old id kept.
- [ ] Check that the old build still activates a computer that never had the new one.
- [ ] Repeat on every platform you ship: macOS (including an Intel build under Rosetta),
      Windows and Linux.
