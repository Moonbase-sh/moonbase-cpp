# [4.0.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v3.3.0...v4.0.0) (2026-08-02)


* feat!: adopt the Moonbase device fingerprint spec v2 for device ids ([#19](https://github.com/Moonbase-sh/moonbase-cpp/issues/19)) ([c7c222a](https://github.com/Moonbase-sh/moonbase-cpp/commit/c7c222a52b462d167c72b729c76e62a5ba6eeb24))


### BREAKING CHANGES

* The default device id now follows the cross-SDK Moonbase device
fingerprint spec (`moonbase:fingerprint:v2`, vendored as FINGERPRINT_SPEC.md and
pinned by tests/vectors/fingerprint-vectors.json). Existing device-bound licenses
must be re-activated, which consumes an activation seat and resets any
device-scoped trial. To avoid that, wrap the default in
moonbase::migrating_device_id_resolver together with
moonbase::legacy_cpp_device_id_resolver and/or
moonbase::juce_integration::legacy_juce_device_id_resolver; see "Migrating from
3.x" in README.md.

moonbase::fingerprint_provider is renamed to moonbase::device_id_resolver and
static_fingerprint_provider to static_device_id_resolver, with deprecated aliases
kept until 5.0.0 (define MOONBASE_DISABLE_DEPRECATED_ALIASES to compile them out).
default_fingerprint_provider now resolves to the spec resolver; the previous
algorithm is preserved verbatim as moonbase::legacy_cpp_device_id_resolver.
licensing::fingerprint() is deprecated in favour of licensing::device_resolver().

A device mismatch now throws moonbase::license_device_mismatch_error
(error_type::license_device_mismatch), which derives from license_invalid_error so
existing catch sites and the offline grace period still behave, but code switching
on error_type::license_invalid must add the case. A machine with no readable
hardware identity now throws moonbase::insufficient_device_identity_error
(error_type::device_identity_unavailable) instead of silently hashing the host
name; opt back in with moonbase_device_id_resolver_options::fallback, or
ActivationConfig::allowDeviceNameFallback, neither of which applies on iOS or
Android where the host name is the same on every device.

* refactor: read iOS and Android identity in the core, not through JUCE

The device fingerprint is meant to be framework-independent, and it was for
macOS, Linux and Windows: the core SDK reads IOKit, sysfs and the firmware table
itself. iOS and Android were the odd ones out, implemented in the JUCE module and
then duplicated into the OnlineUnlockStatus bridge so it had something to use.
Two copies of the same readers, and neither available to a non-JUCE consumer.

Both now live in moonbase_device_id_resolver.hpp alongside the others.
identifierForVendor is reached through the Objective-C runtime's C API, so the
header stays plain C++ with no framework and no .mm file. ANDROID_ID goes through
plain JNI from the NDK.

Android needs one thing a native library genuinely cannot obtain by itself, an
application Context, so the host supplies it once via
moonbase::android::set_jni_environment(vm, context). The JUCE module calls that
from the controller; a non-JUCE app calls it from JNI_OnLoad. Until it is called
Android resolves to insufficient_device_identity_error rather than to a constant,
which is the honest answer for a machine the SDK cannot identify.

Deletes modules/moonbase_licensing/juce/{ios,android}_device_id_resolver.h and
the bridge's copy of them, and collapses
ActivationConfig::defaultDeviceIdResolver() back to a single resolver, since
there is no longer a platform for which the core one is wrong. Net 169 lines
removed from the JUCE surface.

No device id changes: the iOS material is the same identifierForVendor,
normalized the same way, and Android was previously unreachable from the core at
all.

* fix: replace std::call_once memoization, which deadlocks under ThreadSanitizer

The sanitizers workflow was not slow, it was hung. Its log stops dead at
"Start 22: a failed identity read is retried, not cached" and sits there until
the 20-minute timeout kills it. Everything before that is fast: tests 1-21 take
about one second under TSan, including the 8-thread memoization test (0.02s) and
the 20,000-iteration SMBIOS sweep (0.17s). TSan overhead was never the issue.

moonbase_device_id_resolver::description() lets
insufficient_device_identity_error escape the std::call_once callable on purpose,
so a machine that is momentarily unreadable retries instead of caching the
failure. libstdc++ implements call_once on pthread_once, and TSan's pthread_once
interceptor does not model the reset that the exception path performs, so the
*second* call blocks forever. That is exactly what test 22 does, and it is why
ASan and UBSan pass on the same libstdc++ while TSan does not.

Replaces both memos, and the one in migrating_device_id_resolver whose
allocations can also throw, with a mutex and a flag set only after the value is
stored. Same semantics, including the deliberate retry, without depending on the
one corner of call_once that is fragile. Returning a reference stays safe because
neither value is mutated once its flag is set, and the lock provides the
happens-before edge.

Worth fixing in the header rather than skipping the test: any consumer running
TSan against a machine with no readable hardware identity would hit the same
deadlock in shipped code.

# [3.3.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v3.2.0...v3.3.0) (2026-06-29)


### Features

* in-app "Update available" view with release notes and installer download ([#18](https://github.com/Moonbase-sh/moonbase-cpp/issues/18)) ([7aee6e6](https://github.com/Moonbase-sh/moonbase-cpp/commit/7aee6e6f6e8fe2003d2cb0797c58be899d6a7663))

# [3.2.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v3.1.0...v3.2.0) (2026-06-26)


### Bug Fixes

* sync package-lock.json so the release workflow's npm ci succeeds ([#17](https://github.com/Moonbase-sh/moonbase-cpp/issues/17)) ([8f606b3](https://github.com/Moonbase-sh/moonbase-cpp/commit/8f606b3bdf84c72c80aecd439d457e00e61b20f6)), closes [#14](https://github.com/Moonbase-sh/moonbase-cpp/issues/14)


### Features

* actionable HTTP and license-file permission errors ([#16](https://github.com/Moonbase-sh/moonbase-cpp/issues/16)) ([91523c2](https://github.com/Moonbase-sh/moonbase-cpp/commit/91523c2c1554cede957289b665bfd29667007817))
* moonbase_licensing JUCE 8 module with native activation UI ([#14](https://github.com/Moonbase-sh/moonbase-cpp/issues/14)) ([2c434a6](https://github.com/Moonbase-sh/moonbase-cpp/commit/2c434a6662e2878904f3bbff311155a2b4a03d14)), closes [hi#contrast](https://github.com/hi/issues/contrast)

# [3.1.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v3.0.0...v3.1.0) (2026-06-16)


### Features

* add offline activation flow ([#13](https://github.com/Moonbase-sh/moonbase-cpp/issues/13)) ([e680e74](https://github.com/Moonbase-sh/moonbase-cpp/commit/e680e748f3b0e7091fd76a7c6b59674b171c36c4))

# [3.0.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v2.2.0...v3.0.0) (2026-05-18)


* refactor!: improve C++ SDK header hygiene ([#10](https://github.com/Moonbase-sh/moonbase-cpp/issues/10)) ([29aeb56](https://github.com/Moonbase-sh/moonbase-cpp/commit/29aeb562e99735fe5518190f802b4f624a9d679b))


### BREAKING CHANGES

* moonbase::platform::linux was renamed to moonbase::platform::linux_os.

* fix: keep file lock header free of Win32 macros

# [2.2.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v2.1.0...v2.2.0) (2026-05-18)


### Features

* deduplicate online license re-validation across plugin instances ([#11](https://github.com/Moonbase-sh/moonbase-cpp/issues/11)) ([f095246](https://github.com/Moonbase-sh/moonbase-cpp/commit/f09524609ad6cec320f457e811099b61e1b746ac))

# [2.1.0](https://github.com/Moonbase-sh/moonbase-cpp/compare/v2.0.0...v2.1.0) (2026-05-09)


### Features

* add revoke_activation for online-activated licenses ([#8](https://github.com/Moonbase-sh/moonbase-cpp/issues/8)) ([35c27e6](https://github.com/Moonbase-sh/moonbase-cpp/commit/35c27e67f2b6f863e014fd11d7a35570471e7398))

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
