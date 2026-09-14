# Migrating from 3.x

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
can read, so the JUCE module emits a [scoped](../FINGERPRINT_SPEC.md#scoped-identity)
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

## Renamed APIs

4.0.0 also renamed three core SDK names: `fingerprint_provider` is now
`device_id_resolver`, `static_fingerprint_provider` is `static_device_id_resolver`,
and `licensing::fingerprint()` is `licensing::device_resolver()`. The old names remain
as deprecated aliases and will be removed in 5.0.0; define
`MOONBASE_DISABLE_DEPRECATED_ALIASES` to find every remaining use now.
