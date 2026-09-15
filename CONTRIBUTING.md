# Contributing

Thanks for helping out. This page covers how to build and test the repo, the files
that are generated rather than hand-written, and how a change becomes a release.

For using the SDK, start at the [README](README.md).

## Build

```bash
cmake -B build
cmake --build build
```

The [core SDK guide](docs/core-sdk.md#cmake) documents every `MOONBASE_*` option. The
JUCE targets are all opt-in, so a plain configure never fetches JUCE.

## Tests

`scripts/test.sh` configures, builds and runs the doctest suite. Unit tests do not hit
the network; live API tests are opt-in:

```bash
scripts/test.sh
scripts/test.sh --live
scripts/test.sh --clean --build-dir build-debug
```

Live-test defaults target the demo setup used by the Node SDK:

- `MOONBASE_CPP_ENDPOINT`, default `https://demo.moonbase.sh`
- `MOONBASE_CPP_PRODUCT_ID`, default `demo-app`
- `MOONBASE_CPP_PUBLIC_KEY`, default demo public key
- `MOONBASE_CPP_ACCOUNT_ID`, optional issuer check

Live tests create a unique activation request and try to fulfill it through the
anonymous trial endpoint.

The other suites are separate because each pulls in something the core tests do not:

| Suite | Command | Covers |
| --- | --- | --- |
| JUCE module | `cmake -B build -DMOONBASE_BUILD_JUCE_TESTS=ON -DMOONBASE_USE_CURL=OFF && ctest --test-dir build` | `ActivationController` and the native RS256 backends (Security.framework, CNG, libcrypto) |
| Visual snapshots | `scripts/visual-snapshots.sh` | Every activation UI state rendered offscreen; see [`tests/visual/README.md`](tests/visual/README.md) |
| Sanitizers | `cmake -B build-asan -DMOONBASE_SANITIZER=address,undefined` | The controller and license store both spawn threads |
| Installed package | `tests/consumer_smoke/` | That `find_package(moonbase_cpp)` exports correctly |

## Generated and mirrored files

Two paths in this repo are copies, not sources. CI fails if either drifts, so run the
matching script and commit its output in the same change.

| Path | Source of truth | Refresh with |
| --- | --- | --- |
| `modules/moonbase_licensing/moonbase/` | `include/moonbase/` | `scripts/sync-juce-module.sh` |
| `FINGERPRINT_SPEC.md`, `tests/vectors/fingerprint-vectors.json` | `@moonbase.sh/licensing` | `scripts/sync-fingerprint-vectors.sh` |

`sync-juce-module.sh` is a byte-exact mirror with `--delete`, so **never hand-edit
anything under `modules/moonbase_licensing/moonbase/`**; the next sync reverts it. Any
change under `include/moonbase/`, down to a comment, needs a re-run.

`sync-fingerprint-vectors.sh` reads the JavaScript SDK checkout. It is not a sibling
of this repo by default, so point `MOONBASE_JS_ROOT` at it (or pass `--from`).

Both take `--check`, which is exactly what the `consistency` job in
[`ci.yml`](.github/workflows/ci.yml) runs.

## Scripts

| Script | Purpose |
| --- | --- |
| `test.sh` | Configure, build and run the test suite (`--live`, `--clean`, `--build-dir`) |
| `sync-juce-module.sh` | Mirror `include/moonbase/` into the JUCE module (`--check`) |
| `sync-fingerprint-vectors.sh` | Refresh the vendored spec and conformance vectors (`--check`, `--vectors-only`, `--from`) |
| `visual-snapshots.sh` | Build and render the activation UI snapshots (`--upload` to Argos) |
| `gen-nfc-tables.py` | Regenerate the Unicode NFC tables in `include/moonbase/detail/unicode/` |
| `bump-version.sh` | Stamp a new version across the repo. Run by semantic-release, not by hand |

## CI

| Workflow | Runs |
| --- | --- |
| [CI](.github/workflows/ci.yml) | Consistency checks, then build + test + install + consumer smoke on Linux, macOS and Windows |
| [JUCE module](.github/workflows/juce.yml) | The module and its tests on all three platforms, plus JUCE 8.0.0, 7.0.12 and 6.1.3 on macOS; the only job exercising the native crypto backends |
| [Sanitizers](.github/workflows/sanitizers.yml) | ASan + UBSan and TSan over the test suite |
| [Visual tests](.github/workflows/visual.yml) | Renders the UI snapshots and uploads them to Argos when `ARGOS_TOKEN` is set, plus a JUCE 6.1.3 smoke render |
| [Fingerprint parity](.github/workflows/fingerprint-parity.yml) | Proves on real hardware, per OS, that this SDK and `@moonbase.sh/licensing` compute the same device id |
| [PR Title](.github/workflows/pr-title.yml) | Enforces the Conventional Commits PR title |

Run the parity workflow manually (`workflow_dispatch`) after a Node SDK release.

## Pull requests

Pull requests must be merged with **squash merging**, and the PR title must follow
[Conventional Commits](https://www.conventionalcommits.org/). That title becomes the
squash commit on `main` and is what semantic-release reads, which is why the `PR Title`
workflow enforces it on every PR.

- `fix: ...` &rarr; patch (e.g. `0.1.0` &rarr; `0.1.1`)
- `feat: ...` &rarr; minor (e.g. `0.1.0` &rarr; `0.2.0`)
- `feat!: ...` or any commit with a `BREAKING CHANGE:` footer &rarr; major

## Releases

Releases are fully automated by [semantic-release](https://semantic-release.gitbook.io/)
running on every push to `main`. Each release:

- Bumps `VERSION` in `CMakeLists.txt` (which flows into `MOONBASE_CPP_VERSION` and the
  `User-Agent: moonbase-cpp/<version>` header) and the JUCE module's own version in
  `modules/moonbase_licensing/moonbase_licensing.h`
- Rewrites the pinned `GIT_TAG` in `README.md` and [`docs/core-sdk.md`](docs/core-sdk.md)
- Updates `CHANGELOG.md`
- Tags the commit and creates a GitHub Release, with source archives at
  `https://github.com/Moonbase-sh/moonbase-cpp/archive/refs/tags/v<version>.tar.gz`

`scripts/bump-version.sh` does the stamping and **verifies every rewrite**, so it
fails the release rather than shipping a half-bumped tree. If you move or reformat a
version-bearing line, update that script and the `assets` list in `.releaserc.json`
together, or the change is either unstamped or stamped-then-discarded.
