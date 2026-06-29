# Visual tests

Visual regression tests for the `moonbase_licensing` activation UI. They render
the `ActivationComponent` in every state to PNGs **offscreen** (no window, no
network) and upload them to [Argos](https://argos-ci.com) for diffing against a
baseline.

## How it works

[`snapshot_main.cpp`](snapshot_main.cpp) is a small JUCE console app that, for each
state, constructs an `ActivationComponent` and uses two module seams to make the
frame deterministic:

- `ActivationConfig::reduceMotion` — transitions/spinner/pop jump straight to their
  final frame (also a real accessibility option).
- `ActivationController::setPreviewState(screen, license, error)` — forces any
  screen with a synthetic license, no network, no stored state.

It then captures `juce::Component::createComponentSnapshot(...)` at 2× and writes a
PNG per state:

| File | State |
| --- | --- |
| `01-welcome` | Not activated — online / offline |
| `01b-welcome-error` | Activation request failed (error message) |
| `02-activating` | Browser activation in progress (spinner + device chip) |
| `03-success` | Just activated (license card) |
| `04-offline-empty` | Offline flow, nothing chosen yet |
| `05-offline-ready` | Offline flow with a response file selected |
| `05b-offline-error` | Offline flow with an invalid response file (error) |
| `06-trial` | Valid trial license (days-left, progress, features) |
| `06b-trial-expired` | Trial that has ended (locked) |
| `06c-trial-overflow` | Trial with a long feature list (scrollable field) |
| `07-details` | Valid perpetual license (details, seats, deactivate) |
| `07b-details-subscription` | Subscription license with an expiry date |
| `07c-details-error` | Details with a deactivate failure (error) |
| `07d-details-update-available` | Details with a clickable "Update available" badge |
| `08-details-offline` | Offline-activated (permanent) license |
| `09-details-deactivating` | Deactivate in progress (inline spinner) |
| `10-update-loading` | Update available, fetching release notes (skeleton) |
| `11-update-ready` | Update available, notes loaded (download button) |
| `12-update-downloading` | Update installer downloading (progress bar) |
| `13-update-error` | Update details failed to load (error) |

Add a state by adding a `writeSnapshot(...)` call.

## Run locally

```bash
./scripts/visual-snapshots.sh            # build + render into ./ui-snapshots
./scripts/visual-snapshots.sh --upload   # …then upload to Argos (needs ARGOS_TOKEN)
```

On this repo's reference Mac the host toolchain needs an older SDK; pass it through:

```bash
MOONBASE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX14.2.sdk \
  ./scripts/visual-snapshots.sh
```

## CI / Argos

[`.github/workflows/visual.yml`](../../.github/workflows/visual.yml) builds + renders
on macOS (which also exercises the Apple crypto backend), stores the PNGs as a build
artifact, and uploads to Argos when the `ARGOS_TOKEN` repository secret is set.

To enable Argos: create a project at argos-ci.com, connect this repository, and add
its token as the `ARGOS_TOKEN` Actions secret. Argos records the first run as the
baseline and flags pixel diffs on later runs for approval.
