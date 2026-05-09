# Moonbase C++ Activation SDK

Header-only C++17 SDK for Moonbase license activation. It supports activation requests, polling for fulfilled activations, local RS256 JWT validation, overridable device fingerprinting, and overridable license storage.

## CMake

```cmake
find_package(moonbase_cpp REQUIRED)

target_link_libraries(your_app PRIVATE moonbase::licensing)
```

The interface target depends on `CURL::libcurl`, `OpenSSL::SSL`, `OpenSSL::Crypto`, and `nlohmann_json::nlohmann_json`.

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

On startup, validate the stored token locally:

```cpp
if (auto local = licensing.store().load_local_license()) {
    auto validated = licensing.validate_token(local->token);
}
```

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
- Tags the commit and creates a GitHub Release with a `moonbase-cpp-<version>.tar.gz` source tarball attached
