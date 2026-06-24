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

All transitions use JUCE 8's animation API (`juce::Animator` / `ValueAnimatorBuilder`
/ `Easings`, driven by a `VBlankAnimatorUpdater`): cross-fade + fade-up between
screens, the activating spinner, the success pop, and the breathing top-edge glow.

## Gating

```cpp
if (! activation->controller().license().has_value())
    buffer.clear();   // not activated
```

`controller().license()` is the full `moonbase::license` — `trial`, `expires_at`,
`issued_to.email`, `owned_sub_product_ids`, custom `properties`, etc. — for richer
gating.

## Branding / theming

Everything in `ActivationConfig` after the connection fields is brand/UI: product +
manufacturer name, `accent` colour, the Moonbase co-brand badge (`showMoonbaseBadge`), the
`trialLengthDays` + `trialFeatures` list (shown on the Trial / Expired screens),
`enableOffline`, and the `activationUrl`. For deeper re-skinning, mutate `ActivationLookAndFeel::palette`
(every colour is a token) or bundle real Inter / Space Mono typefaces and point the
`heading` / `body` / `mono` font helpers at them.

## Fingerprinting

By default the module identifies the device via
`juce::SystemStats::getUniqueDeviceID()` (so it never shells out to ioreg/dmidecode
inside a sandboxed host). Pick one fingerprint source when you ship and keep it —
changing it changes the device id Moonbase sees and invalidates existing activations.

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
SDK's requests. `includeAppVersion` also fills `application_version` from the running app
when you haven't set it explicitly.
