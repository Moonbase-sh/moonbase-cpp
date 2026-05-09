# Moonbase C++ Activation SDK

Header-only C++17 SDK for Moonbase license activation. It supports activation requests, polling for fulfilled activations, local RS256 JWT validation, overridable device fingerprinting, and overridable license storage.

## Requirements

- CMake 3.20 or newer
- A C++17 compiler
- Windows, macOS, or Linux (the default fingerprint provider has native implementations for each)
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
    GIT_TAG v1.1.0)
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
| `MOONBASE_BUILD_TESTS` | `ON` | Build the doctest-based unit and live tests. |
| `MOONBASE_BUILD_EXAMPLES` | `ON` | Build the standalone activation example under `examples/`. |
| `MOONBASE_BUILD_JUCE_EXAMPLE` | `OFF` | Fetch JUCE and build the JUCE bridge example (see below). |

Set `MOONBASE_BUILD_TESTS` and `MOONBASE_BUILD_EXAMPLES` to `OFF` when integrating via `add_subdirectory` or `FetchContent` to avoid building artifacts you don't need.

## Basic Usage

```cpp
#include <moonbase/moonbase.hpp>

moonbase::licensing_options options;
options.endpoint = "https://demo.moonbase.sh";
options.product_id = "demo-app";
options.public_key = public_key_pem;
options.account_id = "tenant-id"; // optional issuer check

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

## Custom Fingerprinting and Storage

```cpp
class my_fingerprint final : public moonbase::fingerprint_provider {
public:
    std::string device_name() const override { return "Studio Mac"; }
    std::string device_id() const override { return "stable-device-id"; }
};

auto store = std::make_shared<moonbase::file_license_store>("licenses/license.mb");
auto fingerprint = std::make_shared<my_fingerprint>();
moonbase::licensing licensing(options, store, fingerprint);
```

The default store is in-memory. `file_license_store` persists a JSON representation of the validated license.

The default fingerprint provider builds a stable, native hardware fingerprint
from platform identity parameters such as SMBIOS fields on Windows,
`IOPlatformUUID` on macOS, and board/BIOS/CPU fields on Linux. Use a custom
`fingerprint_provider` when you need an exact legacy fingerprint or any other
application-specific device ID.

## JUCE Plugins

For JUCE-based plugins and applications, [`docs/juce.md`](docs/juce.md) walks
through a drop-in bridge that wires Moonbase activation into
`juce::OnlineUnlockStatus`, sources the device fingerprint from JUCE's
`SystemStats` helpers, and populates activation metadata with host/system
context (DAW, plugin format, OS, CPU, JUCE version). The reference code
lives under [`examples/juce/`](examples/juce/) and ships with a runnable
standalone app:

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_EXAMPLE=ON
cmake --build build --target MoonbaseJuceExample
```

The flag is opt-in — JUCE is fetched and compiled only when it's set.

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
