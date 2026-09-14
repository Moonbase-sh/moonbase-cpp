# JUCE module sample

A runnable standalone JUCE app that shows the [`moonbase_licensing`
module](../../modules/moonbase_licensing/) the way a real plugin would use it,
against the public demo environment (`https://demo.moonbase.sh`, product `demo-app`).

It mimics a plugin editor for a fictional "Solstice" plugin and presents
`ActivationComponent` as a **modal overlay** on top of it (`overlayBackdrop = true`).
"Open Solstice", the close button, and a successful activation all dismiss the
overlay to reveal the app underneath; the License button brings it back. That is the
shape most plugins want, so the file doubles as reference wiring.

It also exercises the config surface beyond the three required fields: product and
manufacturer names, an accent colour, `applicationVersion` for the update screen, the
analytics opt-in, a custom `metadata` field, and an `onDiagnostic` sink.

The screenshots in the [module README](../../modules/moonbase_licensing/README.md) are
renders of this app.

## Files

- `Main.cpp`: the JUCEApplication shell, the stand-in editor, and
  `makeConfig()`, the single place every Moonbase setting is filled in.
- `CMakeLists.txt`: builds the app. JUCE and the module itself are added by the
  top-level `CMakeLists.txt`, shared with the UI snapshot target.

## Build

```bash
cmake -B build -DMOONBASE_BUILD_JUCE_NATIVE_EXAMPLE=ON
cmake --build build --target MoonbaseActivationNative
```

The first configure clones JUCE (~80 MB) and the first build compiles it from source
(several minutes). Subsequent builds are fast.

The artefact lands at `build/examples/juce-native/MoonbaseActivationNative_artefacts/`
(an `.app` on macOS, an `.exe` on Windows, a binary on Linux).

See [`docs/juce-module.md`](../../docs/juce-module.md) for the full integration guide.
