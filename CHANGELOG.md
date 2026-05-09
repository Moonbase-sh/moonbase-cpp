# [2.0.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v1.1.0...v2.0.0) (2026-05-09)


* feat!: add validate_token_online with grace period and cadence ([#6](https://github.com/Moonbase-sh/moonbase-cpp/issues/6)) ([fd4780b](https://github.com/Moonbase-sh/moonbase-cpp/commit/fd4780b6e330e99f44652a22899bdd8edf2aa511))


### BREAKING CHANGES

* licensing::validate_token has been renamed to
licensing::validate_token_local to make the local-only semantics explicit
alongside the new validate_token_online.

* docs(juce): use validate_token_online in bridge and document grace/cadence

MoonbaseUnlockStatus::tryLoadStoredLicense now defaults to
validate_token_online, persists the refreshed token so the cadence/grace
clock advances across restarts, and catches transport-past-grace failures
as "not unlocked" instead of letting them propagate into the host. Adds an
online=false escape hatch for callers that need pure local validation.

Updates docs/juce.md to describe the new defaults, the two licensing_options
knobs (online_validation_min_interval, online_validation_grace_period), the
offline-token guarantee, and the synchronous-call caveat.

* feat(juce): add async tryLoadStoredLicenseAsync that never blocks the host

The synchronous tryLoadStoredLicense path is fine for CLI tools and
standalone apps but a real plugin can't afford to block the host's
plugin-load thread on libcurl. The new async variant runs local validation
inline (so the plugin loads optimistically unlocked from cached state) and
performs the online check on a juce::Thread, marshalling the result back to
the message thread via callAsync. A juce::WeakReference protects the
continuation from a destroyed bridge, and licensing_ is now held via
shared_ptr so the background thread can safely outlive a teardown
mid-request.

The result enum (Refreshed / LockedInvalid / LockedExpired / Unreachable /
OfflineToken / NoStoredLicense / LocalInvalid) lets UI code distinguish
"server unreachable past grace" from "license revoked" if it cares; for
most callers, just calling refreshLabel() (or equivalent) on the bridge's
unlock state is enough.

PluginActivationComponent now uses the async variant. docs/juce.md
documents both code paths and recommends async for plugins.

* fix: address review on grace/throttle interaction and async correctness

- validate_token_online's throttle skip now requires the token age to be
  within both online_validation_min_interval AND online_validation_grace_period.
  Previously a min_interval longer than the grace period silently extended
  "max age without an online check" past its advertised limit (e.g. min=30d,
  grace=7d would never revalidate during days 1-29). Adds a test that pins
  the new behavior with min_interval > grace_period.

- tryLoadStoredLicenseAsync now always marshals state mutation and the
  callback through juce::MessageManager::callAsync, including the
  early-return paths (NoStoredLicense, LocalInvalid, OfflineToken). The doc
  promised message-thread delivery but those cases fired synchronously on
  the caller's thread, which is a problem because hosts often construct
  AudioProcessors off the message thread.

- Adds an atomic generation counter on the bridge. tryLoadStoredLicense*,
  pollPendingActivation (on success), and clearLicense bump it; async
  continuations capture the value at request time and silently drop both
  state mutation and callback if a newer call has superseded them. This
  prevents a slow online check from resurrecting a license the user just
  cleared, or clobbering a freshly activated one.

* docs(juce): persist license to disk and surface validated_at in the example

The standalone example app now wires a file_license_store under the
platform's per-user app data directory (so activation actually survives
restart) and displays the license's validated_at claim alongside the email
and expiry — handy for seeing the cadence/grace clock advance on each
launch.

# [1.1.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v1.0.1...v1.1.0) (2026-05-09)


### Features

* add JUCE integration bridge with runnable example ([#4](https://github.com/Moonbase-sh/moonbase-cpp/issues/4)) ([b62c506](https://github.com/Moonbase-sh/moonbase-cpp/commit/b62c506b1ae879251f8e7e99854e298e2422f3dd))

## [1.0.1](https://github.com/Moonbase-sh/moonbase-cpp/compare/v1.0.0...v1.0.1) (2026-05-09)


### Bug Fixes

* register each doctest case with CTest individually ([#3](https://github.com/Moonbase-sh/moonbase-cpp/issues/3)) ([0fd4669](https://github.com/Moonbase-sh/moonbase-cpp/commit/0fd466912800c130f9cb4050ddd6768d0db1c4b9))

# 1.0.0 (2026-05-09)


### Features

* initial Moonbase C++ activation SDK ([#1](https://github.com/Moonbase-sh/moonbase-cpp/issues/1)) ([66d9722](https://github.com/Moonbase-sh/moonbase-cpp/commit/66d9722796ca14d13ab98b3f99b96f37ce14ad71))
