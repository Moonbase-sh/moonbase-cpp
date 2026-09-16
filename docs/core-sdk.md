# Core SDK: `moonbase::licensing`

The header-only C++17 library at [`include/moonbase/`](../include/moonbase/). It
covers activation requests, polling for fulfilled activations, local RS256 JWT
validation, cross-SDK device fingerprinting, offline (machine-file) activation,
revocation, and overridable license storage. No UI, no framework, no build step.

Building a JUCE plugin? The [`moonbase_licensing` module](juce-module.md) wraps this
same core and adds a ready-made activation UI.

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
    GIT_TAG v4.3.1)
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

The build exposes these options. Everything except the SDK target itself defaults
off when the project is consumed as a subproject, so a `FetchContent` or
`add_subdirectory` integration never silently acquires test or application targets.

| Option | Default | Purpose |
| --- | --- | --- |
| `MOONBASE_USE_CURL` | `ON` | Build the SDK target with the libcurl HTTP transport. Off drops the CURL dependency entirely; the interface then defines `MOONBASE_DISABLE_CURL_TRANSPORT=1` and you supply your own `http_client`. |
| `MOONBASE_BUILD_TESTS` | `ON` top-level, `OFF` as a subproject | Build the doctest-based unit and [live tests](../CONTRIBUTING.md#tests). |
| `MOONBASE_BUILD_EXAMPLES` | `ON` top-level, `OFF` as a subproject | Build the standalone activation example under `examples/`. |
| `MOONBASE_BUILD_DEVICE_ID_TOOL` | `ON` top-level, `OFF` as a subproject | Build the `moonbase_device_id` diagnostic, which prints this machine's device id and how it was derived. |
| `MOONBASE_BUILD_JUCE_EXAMPLE` | `OFF` | Fetch JUCE and build the [`OnlineUnlockStatus` bridge](juce.md) sample. |
| `MOONBASE_BUILD_JUCE_NATIVE_EXAMPLE` | `OFF` | Fetch JUCE and build the [`moonbase_licensing` module](juce-module.md) sample. |
| `MOONBASE_BUILD_JUCE_TESTS` | `OFF` | Fetch JUCE and doctest and build the JUCE module's test suite (`tests/juce/`). |
| `MOONBASE_BUILD_UI_SNAPSHOTS` | `OFF` | Fetch JUCE and build the [offscreen UI snapshot harness](../tests/visual/README.md). |
| `MOONBASE_SANITIZER` | *(empty)* | Sanitizer for the test targets: `address`, `thread`, `undefined`, or `address,undefined`. Ignored under MSVC. |
| `MOONBASE_JUCE_VERSION` | `8.0.4` | JUCE tag the four JUCE options above fetch. Anything from `6.1.3` up works. It only exists as a cache variable once one of them is on. |

Override `MOONBASE_BUILD_TESTS` and `MOONBASE_BUILD_EXAMPLES` explicitly when you want
a subproject integration to build SDK artifacts too. The four JUCE options are opt-in
everywhere: JUCE is fetched and compiled only when one of them is set.

## Basic Usage

```cpp
#include <moonbase/moonbase.hpp>

moonbase::licensing_options options;
options.endpoint = "https://demo.moonbase.sh";
options.product_id = "demo-app";
options.public_key = public_key_pem;
options.account_id = "tenant-id"; // optional issuer check
options.client_info = "my-framework/1.2.0"; // optional, see below
options.http_connect_timeout = std::chrono::seconds(10);
options.http_request_timeout = std::chrono::seconds(30);

moonbase::licensing licensing(options);
```

`client_info` identifies a higher-level integration built on top of the SDK (the
JUCE module sets `moonbase-juce/<version> (JUCE …; OS)`, for example). It is
appended to the `User-Agent` after `moonbase-cpp/<version>`, so requests report
every layer, outermost last:

```
User-Agent: moonbase-cpp/4.3.1 my-framework/1.2.0
```

Use product tokens (`Name/Version`, with an optional `(comment)`) and keep it
ASCII. If your code sits on top of another integration that already set it,
append a segment rather than replacing the string. Control characters are
stripped and the value is capped at 256 characters when the header is built, so
a stray newline can never inject a header.

```cpp
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
[`mbd2_` stamp format](device-identity.md), and it gives up cross-SDK compatibility by
definition. If you
include narrow SDK headers instead of `<moonbase/moonbase.hpp>`, include
`<moonbase/moonbase_device_id_resolver.hpp>` for the default resolver and
`<moonbase/http_curl.hpp>` for the default CURL transport.

> **Renamed in 4.0.0.** `fingerprint_provider`, `static_fingerprint_provider` and
> `licensing::fingerprint()` have new names; the old ones remain as deprecated
> aliases until 5.0.0. See [Renamed APIs](migration-3x.md#renamed-apis).
