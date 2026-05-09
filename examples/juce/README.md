# JUCE bridge sample

A runnable standalone JUCE app that exercises the Moonbase activation flow
against the public demo environment (`https://demo.moonbase.sh`, product
`demo-app`). The same files double as drop-in reference code for your own
plugin.

## Files

- `MoonbaseJuceBridge.h` — the bridge: fingerprint provider, metadata helper,
  and `MoonbaseUnlockStatus` (a `juce::OnlineUnlockStatus` subclass).
- `PluginActivationComponent.h` — a `juce::Component` that drives activation
  end to end. Wires up demo endpoint/product/public-key.
- `Main.cpp` — JUCEApplication entry point that opens a window hosting the
  component.
- `CMakeLists.txt` — builds the standalone app, fetching JUCE on first
  configure.

## Build

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_EXAMPLE=ON
cmake --build build --target MoonbaseJuceExample
```

The first configure clones JUCE (~80 MB) and the first build compiles JUCE
from source (several minutes). Subsequent builds are fast.

The artefact lands at `build/examples/juce/MoonbaseJuceExample_artefacts/`
(an `.app` on macOS, an `.exe` on Windows, a binary on Linux).

See [`docs/juce.md`](../../docs/juce.md) for the integration guide.
