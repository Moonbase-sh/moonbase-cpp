# Moonbase C++ Activation SDK

Header-only C++17 SDK for Moonbase license activation. It supports activation requests, polling for fulfilled activations, local RS256 JWT validation, cross-SDK device fingerprinting (spec v2), and overridable license storage.

## Requirements

- CMake 3.20 or newer
- A C++17 compiler
- Windows, macOS, or Linux (the default device id resolver has native implementations for each)
- `CURL::libcurl` and OpenSSL (`OpenSSL::SSL`, `OpenSSL::Crypto`) — must be findable on the system (e.g. via your distro, Homebrew, or vcpkg)
- `nlohmann_json` 3.11+ — used if `find_package(nlohmann_json)` succeeds; otherwise it is fetched automatically at build time from the upstream release tarball

The installed package config calls `find_dependency()` for CURL, OpenSSL, and nlohmann_json, so a consuming project does not need to repeat those `find_package` calls itself — but the libraries must be available when `find_package(moonbase_cpp)` is invoked.

## Installation

### Install from source

Clone the repository (or download a release tarball at `https://github.com/Moonbase-sh/moonbase-cpp/archive/refs/tags/v<version>.tar.gz`), then configure, build, and install:

```bash
cmake -B build -DMOONBASE_BUILD_TESTS=OFF -DMOONBASE_BUILD_EXAMPLES=OFF
cmake --build build
cmake --install build --prefix /your/prefix
```

### FetchContent

To pull the SDK into your own CMake build without a separate install step:

```cmake
include(FetchContent)
FetchContent_Declare(moonbase_cpp
    GIT_REPOSITORY https://github.com/Moonbase-sh/moonbase-cpp.git
    GIT_TAG v4.0.0)
set(MOONBASE_BUILD_TESTS OFF)
set(MOONBASE_BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(moonbase_cpp)

target_link_libraries(your_app PRIVATE moonbase::licensing)
```

`add_subdirectory()` works the same way if you vendor the source tree into your repo.

## CMake

```cmake
find_package(moonbase_cpp REQUIRED)

target_link_libraries(your_app PRIVATE moonbase::licensing)
```

The package exports the `moonbase::licensing` interface target, which propagates the include directory along with `CURL::libcurl`, `OpenSSL::SSL`, `OpenSSL::Crypto`, and `nlohmann_json::nlohmann_json` as transitive dependencies.

The build provides three options, all useful when consuming the SDK as a subproject:

| Option | Default | Purpose |
| --- | --- | --- |
| `MOONBASE_BUILD_TESTS` | `ON` for the top-level project, `OFF` as a subproject | Build the doctest-based unit and live tests. |
| `MOONBASE_BUILD_EXAMPLES` | `ON` for the top-level project, `OFF` as a subproject | Build the standalone activation example under `examples/`. |
| `MOONBASE_BUILD_JUCE_EXAMPLE` | `OFF` | Fetch JUCE and build the JUCE `OnlineUnlockStatus` bridge example (see below). |
| `MOONBASE_BUILD_JUCE_NATIVE_EXAMPLE` | `OFF` | Fetch JUCE and build the `moonbase_licensing` native module example (see below). |
| `MOONBASE_BUILD_DEVICE_ID_TOOL` | `ON` for the top-level project, `OFF` as a subproject | Build the `moonbase_device_id` diagnostic, which prints this machine's device id and how it was derived. |

Override `MOONBASE_BUILD_TESTS` and `MOONBASE_BUILD_EXAMPLES` explicitly when you want a subproject integration to build SDK artifacts too.

## Basic Usage

```cpp
#include <moonbase/moonbase.hpp>

moonbase::licensing_options options;
options.endpoint = "https://demo.moonbase.sh";
options.product_id = "demo-app";
options.public_key = public_key_pem;
options.account_id = "tenant-id"; // optional issuer check
options.http_connect_timeout = std::chrono::seconds(10);
options.http_request_timeout = std::chrono::seconds(30);

moonbase::licensing licensing(options);

auto request = licensing.request_activation();
std::cout << "Open: " << request.browser_url << "\n";

std::optional<moonbase::license> license;
while (!license) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    license = licensing.get_requested_activation(request);
}

licensing.store().store_local_license(*license);
```

`request_activation` takes an optional `moonbase::activation_method`. Pass
`activation_method::offline` to have the same browser flow mint an *offline*
license instead:

```cpp
auto request = licensing.request_activation(moonbase::activation_method::offline);
```

The URL, the polling and the storage step are unchanged, but the resulting token
carries `method: Offline`, so it is validated locally for good
(`validate_token_online` short-circuits it) and
[cannot be revoked](#revoking-an-activation). The product must have offline
activations enabled in Moonbase, otherwise the call throws
`license_invalid_error` reading "Product does not allow offline activations".

On startup, validate the stored token. `validate_token_online` runs the local
checks (signature, device fingerprint, expiry) and then re-validates against the
Moonbase API when needed:

```cpp
if (auto local = licensing.store().load_local_license()) {
    auto validated = licensing.validate_token_online(local->token);
    licensing.store().store_local_license(validated); // persist refreshed token
}
```

Two `licensing_options` knobs control how often the API is contacted and how
much offline tolerance is allowed:

- `online_validation_min_interval` (default 5 minutes) — if the local
  `validated_at` is newer than this, the API call is skipped. Makes the method
  cheap to call frequently (e.g. on every plugin instantiation).
- `online_validation_grace_period` (default 7 days) — maximum age the local
  token may reach without a successful online check. Within grace, transient
  API failures (network down, 5xx, etc.) fall back to the local result. Beyond
  grace, the failure is propagated.

Definitive server rejections (`license_invalid_error`, `license_expired_error`)
always propagate regardless of grace.

Offline-activated tokens (`activation_method::offline`) are validated locally
even when calling `validate_token_online` — the SDK never contacts the API for
them. Use `validate_token_local` directly when you want the local-only check
explicitly.

## Revoking an Activation

To free up the activation seat for the current device — typically wired to a
"Deactivate" or "Sign out" button — call `revoke_activation` with the JWT:

```cpp
if (auto local = licensing.store().load_local_license()) {
    licensing.revoke_activation(local->token); // server-side revoke + clears local store
}
```

On success the SDK both tells the server to release the seat and deletes the
matching license from the local store. Revoke is only meaningful for
online-activated paid licenses; calling it for offline or trial tokens raises
`operation_not_supported_error` without contacting the API. Server rejections
(`license_invalid_error`) and transport failures (`api_error`) propagate the
same way they do for `validate_token_online`, but with no grace-period
fallback — revoke is a one-shot operation.

## Offline Activation

There are two routes to an offline license, and which one fits depends on
whether the machine has network *at activation time*:

- **It does:** run the normal browser activation and ask for an offline license
  with [`request_activation(activation_method::offline)`](#basic-usage).
  Nothing else about the flow changes.
- **It does not:** use the file-based exchange below, which involves no network
  on the device at all.

Either way the resulting token is permanent and unrevokable; it stays valid until
the machine's device fingerprint changes.

For machines without internet access, Moonbase supports a file-based flow: the
app emits a **device token** ("machine file"), the user exchanges it for a
license token on the Moonbase activation page, and the app reads that token back
in. No network is involved on the device.

1. Generate the device token and write it to a file (conventionally `.dt`):

   ```cpp
   const auto device_token = licensing.generate_device_token();
   std::ofstream("device-token.dt") << device_token;
   ```

2. The user uploads `device-token.dt` and receives a license token file (the
   raw JWT, conventionally `license.mb`) in return. They can do this through any
   of:

   - **Moonbase's hosted portal** — `https://<your-tenant>.moonbase.sh/activate`.
   - **The embedded storefront on your own site** — trigger the
     [`activate_product`](https://moonbase.sh/docs/storefronts/embedded/#call-methods)
     intent (`Moonbase.activate_product()`), which prompts for the device token
     and hands back the license token file. The `deviceTokenFileExtension`
     (default `.dt`) and `licenseTokenFileName` (default `license-file.mb`)
     config options control the file types involved.
   - **Your own custom flow** — drive the exchange yourself with the Moonbase
     [APIs and SDKs](https://moonbase.sh/docs/licensing/offline-activations/)
     (the `/api/customer/inventory/activate` endpoint).

3. Read the downloaded token back in, validate it locally, and persist it:

   ```cpp
   std::ifstream file("license.mb");
   const std::string token((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

   auto license = licensing.read_offline_license(token); // local validation only
   licensing.store().store_local_license(license);
   ```

`read_offline_license` runs the same local checks as `validate_token_local`
(signature, audience, issuer, device fingerprint, expiry) and additionally
requires the token to have been issued via offline activation, throwing
`license_invalid_error` otherwise. On startup, validate the stored token with
`validate_token_local` — offline tokens are never re-validated against the API
and [cannot be revoked](#revoking-an-activation); they stay valid until the
machine's device fingerprint changes.

## Device fingerprint

Every license is bound to the machine via a device id, stored in the token's `sig`
claim and re-checked on every local validation. The default
`moonbase_device_id_resolver` computes it from the cross-SDK
**[device fingerprint spec](FINGERPRINT_SPEC.md)** (`moonbase:fingerprint:v2`): a
SHA-256 of stable native hardware identifiers, stamped with the spec version.

```
mbd2_9f3c…            // 'mbd' + version + '_' + 64 lowercase hex characters
```

Sources are SMBIOS on Windows, `IOPlatformUUID` via IOKit on macOS, and
`machine-id` plus world-readable DMI on Linux. No subprocess is spawned and no
root-only file is read, so the id is the same elevated or not, and the resolver
works inside an App Sandbox and a plugin host.

The algorithm is language-neutral by design: any Moonbase SDK that implements the
spec and passes the shipped
[`fingerprint-vectors.json`](tests/vectors/fingerprint-vectors.json) computes the
same id on a given machine, so a license activated by one validates in the others.
Adoption is per-SDK: **this SDK conforms from 4.0.0; `@moonbase.sh/licensing`
conforms from 3.0.0.** Check the version of whichever SDK you are pairing with
before relying on it.

The id survives a rename, a locale change, a firmware update, a vCPU resize, and
running with or without elevated privileges. The spec's **stability contract** is
the definitive list. Read it before shipping, along with the two Linux exceptions,
which exist because every per-unit hardware serial is root-only there and the id is
therefore tied to the OS installation rather than the hardware:

- A Linux **OS reinstall** requires re-activation.
- A Linux **VM cloned without clearing `/etc/machine-id`** keeps its device id, so
  a license copied with the disk keeps validating. `machine-id(5)` requires
  reusable images to ship that file empty; when they do, clones behave correctly.
  The SDK cannot detect a badly-prepared image, because the value that would
  distinguish the instances is root-only.

Because the version is part of the id, a mismatch is diagnosable. `validate_token`
throws `license_device_mismatch_error` either way, and the message says which case
you are in:

```cpp
try {
    licensing.validator().validate_token(token);
} catch (const moonbase::moonbase_error& ex) {
    if (ex.type() == moonbase::error_type::license_device_mismatch)
        std::cerr << ex.what(); // 'not for this device', plus any version difference
}
```

### When there is no hardware identity

The resolver throws `insufficient_device_identity_error` rather than falling back
to something weak, in two cases:

- **Nothing readable.** A locked-down process, or a platform with no defined
  parameters (Android, BSD).
- **Only model-level values readable.** Vendor, product and board names are
  byte-identical across every unit of a product line, so fingerprinting them would
  let those machines validate one another's licenses. In practice: a Linux install
  with no `machine-id`, or a machine whose SMBIOS carries an unset UUID *and* a
  blank or filler baseboard serial, the usual shape of a cloned VM image.

Opt in explicitly if a weaker id beats none. Those ids are stamped `mbd2n_` so the
server can tell them apart:

```cpp
moonbase::moonbase_device_id_resolver_options resolver_options;
resolver_options.fallback = moonbase::device_id_fallback::device_name;
auto resolver = std::make_shared<moonbase::moonbase_device_id_resolver>(resolver_options);
```

### Diagnostics and parity checks

`describe_device()` returns the id, spec version, platform tag and the *names* of
the parameters that contributed. It is safe to log or attach to a support ticket,
and returns a fresh copy each call so editing it cannot disturb the binding.

```cpp
if (const auto described = licensing.describe_device())
    std::cout << described->device_id << " (" << described->platform << ")\n";
```

Parameter values are never exposed there, and neither are per-parameter hashes.
They are hardware serial numbers, and an unsalted per-value digest is no safer to
publish than the value, since low-entropy values such as host names or sequential
serials fall to a dictionary. Which parameters contributed is the useful
diagnostic; what they read is not.

The device id itself is a one-way hash of all of them together, so it discloses no
individual serial. It is, however, **derived identically for every
Moonbase-powered product**. The material contains no product- or account-specific
input, so the same machine yields the same device id everywhere, and merchants
receive that string through the integration API and webhooks. Treat it as a stable
cross-vendor machine identifier. That is more than `machine-id(5)` intends, which
asks that the Linux machine id only leave the host through an
*application-specific keyed* derivation. If that matters for your deployment,
supply a custom `device_id_resolver` that mixes in a key of your own.

The lower-level `build_fingerprint_material`, `fingerprint_digest`,
`fingerprint_device_id` and `parse_device_id_stamp` helpers are exported from
`<moonbase/fingerprint_spec.hpp>` so you can verify cross-SDK parity against the
vector file. `examples/device_id.cpp` builds as the `moonbase_device_id` target and
prints all of the above as JSON, which is what
[the parity workflow](.github/workflows/fingerprint-parity.yml) compares against
`@moonbase.sh/licensing` on every OS.

## Migrating from 3.x

Device ids computed by 3.x do not follow the spec, so **by default every device
must re-activate once** after you upgrade. That is not free: a new device id
consumes a fresh activation seat (the old one is not reclaimed) and resets any
device-scoped trial. On a license with few seats, a fleet-wide upgrade can exhaust
them immediately.

Three options, in increasing order of effort:

**1. Let devices re-activate (default).** Simplest, and the id is correct from then
on. Catch `error_type::license_device_mismatch` and call `request_activation()`.
Best when seats are generous or the install base is small.

**2. Accept the old id while binding the new one (recommended for existing
fleets).** `migrating_device_id_resolver` keeps recognising ids this device was
previously bound to, without ever issuing one:

```cpp
auto resolver = std::make_shared<moonbase::migrating_device_id_resolver>(
    std::make_shared<moonbase::moonbase_device_id_resolver>(),  // always what a new activation binds
    std::make_shared<moonbase::legacy_cpp_device_id_resolver>()); // additionally accepted at validation

moonbase::licensing licensing(options, store, resolver);
```

Existing licenses keep validating untouched, while anything newly activated binds
the current fingerprint. The fleet migrates as devices naturally re-activate, with
no flag day and no seat churn. The legacy id is computed lazily, only when the fast
comparison fails, and then memoized, so apps on the happy path pay nothing. Drop
the wrapper in a later release to finish the migration.

**Which legacy resolver to name depends on which integration path you shipped**,
and this is the one thing to get right:

| You shipped | Historical resolver |
| --- | --- |
| The core SDK's default | `moonbase::legacy_cpp_device_id_resolver` (`<moonbase/legacy_fingerprint.hpp>`) |
| The `moonbase_licensing` JUCE module | `moonbase::juce_integration::legacy_juce_device_id_resolver` |
| The `OnlineUnlockStatus` bridge | `MoonbaseJuceDeviceIdResolver` from your copy of `MoonbaseJuceBridge.h` |
| More than one, or you are not sure | Pass all of them |

iOS and Android need migrating too. Neither has an identifier that unrelated apps
can read, so the JUCE module emits a [scoped](FINGERPRINT_SPEC.md#scoped-identity)
id there, stamped `mbd2s_` and derived from `identifierForVendor` or `ANDROID_ID`:
stable for the device within the platform's own scope, and deliberately never
correlated across scopes. That is still a different value from the raw id bound
before 4.0.0, so name `legacy_juce_device_id_resolver` as a historical resolver on
mobile as well.

The wrapper takes any number of historical resolvers, and the only cost of an extra
one is a single lazy hardware read on the mismatch path, so "pass both if unsure"
is the safe advice. Note that the JUCE resolver derives its id from
`juce::SystemStats::getUniqueDeviceID()`, which is not a published stable format,
so it only vouches for a binding if your plugin still ships the JUCE version that
created it.

**3. Stay on the old id.** Pin `legacy_cpp_device_id_resolver` as the current
resolver. Nothing changes, but you keep the old algorithm's defects (on Linux the
id depended on whether the process ran elevated; on Windows the SMBIOS read never
succeeded, so the id silently degraded to a hash of the computer name and renaming
a PC invalidated its license) and you get no cross-SDK compatibility. Use this only
as a short-term hold.

> Options 1 and 2 both recompute every accepted id from the machine's own hardware
> on each call. Nothing about a device binding is ever read from disk, so widening
> what a validator accepts does not widen what an attacker can assert.

## Custom storage and device resolvers

```cpp
class my_resolver final : public moonbase::device_id_resolver {
public:
    std::string device_name() const override { return "Studio Mac"; }
    std::string device_id() const override { return "stable-device-id"; }
};

auto store = std::make_shared<moonbase::file_license_store>("licenses/license.mb");
auto resolver = std::make_shared<my_resolver>();
moonbase::licensing licensing(options, store, resolver);
```

The default store is in-memory. `file_license_store` persists a JSON representation of the validated license.

A custom resolver's id is compared literally, so it does not need to follow the
`mbd2_` stamp format, and it gives up cross-SDK compatibility by definition. If you
include narrow SDK headers instead of `<moonbase/moonbase.hpp>`, include
`<moonbase/moonbase_device_id_resolver.hpp>` for the default resolver and
`<moonbase/http_curl.hpp>` for the default CURL transport.

> **Renamed in 4.0.0.** `fingerprint_provider` is now `device_id_resolver`,
> `static_fingerprint_provider` is `static_device_id_resolver`, and
> `licensing::fingerprint()` is `licensing::device_resolver()`. The old names remain
> as deprecated aliases and will be removed in 5.0.0; define
> `MOONBASE_DISABLE_DEPRECATED_ALIASES` to find every remaining use now.

## JUCE Plugins

For JUCE-based plugins and applications there are two integration paths, both
built on the same `moonbase::licensing` core SDK. The native
[`moonbase_licensing`](docs/juce-module.md) module is the recommended choice for
new projects; the [`juce::OnlineUnlockStatus` bridge](docs/juce.md) remains
available and unchanged.

| | Native module | `OnlineUnlockStatus` bridge |
| --- | --- | --- |
| **Form** | Drop-in JUCE module | Copy-paste reference header |
| **Built-in UI** | Yes (polished, animated, themeable) | No (you build it) |
| **JUCE integration** | Native Moonbase API | `juce::OnlineUnlockStatus` wrapper |
| **JUCE version** | 8.0.4+ | 7+ |
| **Device fingerprint** | Spec v2 (`mbd2_`), cross-SDK; scoped `mbd2s_` on mobile | Spec v2 (`mbd2_`), cross-SDK; scoped `mbd2s_` on mobile |
| **Third-party deps** | None (JUCE `WebInputStream` HTTP, bundled `nlohmann/json`, OS-native RS256) | Inherits the core SDK's CURL + OpenSSL |
| **Entry point** | `ActivationComponent` / `ActivationDialog` | `MoonbaseUnlockStatus` |
| **Best for** | New plugins wanting a ready-made UI | Apps already on `OnlineUnlockStatus`, or JUCE 7 |
| **Docs** | [`docs/juce-module.md`](docs/juce-module.md) | [`docs/juce.md`](docs/juce.md) |

### Native module: `moonbase_licensing`

A drop-in JUCE 8 module that adds Moonbase activation, plus a built-in
themeable activation UI, to any app or plugin. It talks to the Moonbase
licensing API natively (it does not use `juce::OnlineUnlockStatus`) and has no
third-party dependencies: HTTP goes through `juce::WebInputStream`, JSON is a
bundled `nlohmann/json`, and RS256 verification uses the OS-native crypto
(Security.framework, CNG/bcrypt, libcrypto). The module lives at
[`modules/moonbase_licensing/`](modules/moonbase_licensing/); add it with
`juce_add_module()` (or via Projucer), fill in three config fields, and show one
`ActivationComponent`. See [`docs/juce-module.md`](docs/juce-module.md) for the
full guide.

<p align="center">
  <img src="assets/moonbase-juce-license.png" width="600"
       alt="The moonbase_licensing activation UI showing license details: licensed-to name, email, plan, activation type, expiry, seat count, and a Deactivate this device button.">
</p>

Build the in-repo sample app with:

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_NATIVE_EXAMPLE=ON
cmake --build build --target MoonbaseActivationNative
```

#### Reference implementation: DRIFT

[**DRIFT by Corino**](https://github.com/Moonbase-sh/corino-drift) is a JUCE 8
VST3 / AU / Standalone plugin built as a reference implementation of this module
(the native-module counterpart to [HALO](https://github.com/Moonbase-sh/corino-halo),
which uses the bridge). Its knobs are real, automatable parameters that
deliberately don't process audio; the point is the activation workflow wrapped
around a real plugin:

- The processor owns the headless `ActivationController` as the single source of
  truth and calls `start()` to load + validate any stored license on a background
  thread, updating the audio-thread-safe `licensedFlag()`.
- `LicenseGate` reads that lock-free flag in `processBlock` and fades to silence
  when unlicensed (and back up when licensed), so gating never clicks.
- The editor shares the processor's controller and shows the module's brandable
  `ActivationComponent` as a modal overlay (`overlayBackdrop = true`), opened from
  a "Manage License" button via `appear()`; `onActivationChanged` keeps the UI in
  sync with no re-wiring.
- Every connection, branding, trial and telemetry field lives in one
  `makeDriftActivationConfig()` factory shared by the processor and the editor.
- GitHub Actions CI + release pipelines build the plugin bundles across platforms.

DRIFT consumes the module via `FetchContent` + `juce_add_module()` (no git
submodule). See
[`src/Licensing.h`](https://github.com/Moonbase-sh/corino-drift/blob/main/src/Licensing.h)
for the single config point and its
[`CMakeLists.txt`](https://github.com/Moonbase-sh/corino-drift/blob/main/CMakeLists.txt)
for the full wiring.

### `OnlineUnlockStatus` bridge

A drop-in bridge ([`docs/juce.md`](docs/juce.md)) that wires Moonbase activation
into `juce::OnlineUnlockStatus`, uses the same spec device id as the rest of the
SDK, and populates activation metadata with host/system
context (DAW, plugin format, OS, CPU, JUCE version). The bridge header lives at
[`examples/juce/MoonbaseJuceBridge.h`](examples/juce/MoonbaseJuceBridge.h) and is
copy-pasteable into any JUCE project; you supply your own activation UI.

Build the in-repo sample app with:

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_EXAMPLE=ON
cmake --build build --target MoonbaseJuceExample
```

The flag is opt-in: JUCE is fetched and compiled only when it's set (the same
applies to `MOONBASE_BUILD_JUCE_NATIVE_EXAMPLE`).

#### Reference implementation: HALO

[**HALO by Corino**](https://github.com/Moonbase-sh/corino-halo) is a JUCE 8
standalone GUI application built specifically as a reference implementation
of this SDK. It's a saturator-styled app that doesn't actually process
audio; the entire point is the license-gate workflow around it:

- Startup runs a synchronous local JWT check, then re-validates against the
  Moonbase API on a background thread via `tryLoadStoredLicenseAsync`.
- Browser activation handshake with 1-second `juce::Timer` polling.
- "Sign out" menu item wired to `revokeActivationAsync` with a graceful
  `NotRevokable` fallback to a local-only forget.
- `file_license_store` persisted under the platform's per-user app data
  directory.
- GitHub Actions release pipeline that builds on macOS + Windows and
  publishes binaries straight to a Moonbase tenant.

HALO vendors the bridge header verbatim from this repo and consumes
`moonbase::licensing` via `FetchContent`. See
[`src/license/HaloLicenseBridge.cpp`](https://github.com/Moonbase-sh/corino-halo/blob/main/src/license/HaloLicenseBridge.cpp)
and its [`CMakeLists.txt`](https://github.com/Moonbase-sh/corino-halo/blob/main/CMakeLists.txt)
for the full wiring.

## Live Tests

Unit tests do not hit the network. Live API tests are opt-in:

```bash
scripts/test.sh
scripts/test.sh --live
```

Defaults target the demo setup used by the Node SDK:

- `MOONBASE_CPP_ENDPOINT`, default `https://demo.moonbase.sh`
- `MOONBASE_CPP_PRODUCT_ID`, default `demo-app`
- `MOONBASE_CPP_PUBLIC_KEY`, default demo public key
- `MOONBASE_CPP_ACCOUNT_ID`, optional issuer check

Live tests create a unique activation request and try to fulfill it through the anonymous trial endpoint.

## Releases

Releases are fully automated by [semantic-release](https://semantic-release.gitbook.io/) running on every push to `main`. The next version is determined by [Conventional Commits](https://www.conventionalcommits.org/):

- `fix: ...` &rarr; patch (e.g. `0.1.0` &rarr; `0.1.1`)
- `feat: ...` &rarr; minor (e.g. `0.1.0` &rarr; `0.2.0`)
- `feat!: ...` or any commit with a `BREAKING CHANGE:` footer &rarr; major

Pull requests must be merged with **squash merging**, and the PR title must follow Conventional Commits — that title becomes the squash commit on `main` and is what semantic-release reads. The `PR Title` workflow enforces this on every PR.

Each release:

- Bumps `VERSION` in `CMakeLists.txt` (which flows into `MOONBASE_CPP_VERSION` and the `User-Agent: moonbase-cpp/<version>` header)
- Updates `CHANGELOG.md`
- Tags the commit and creates a GitHub Release (with the auto-generated source archives at `https://github.com/<owner>/<repo>/archive/refs/tags/v<version>.tar.gz`)

## License

Released under the [MIT License](LICENSE).
