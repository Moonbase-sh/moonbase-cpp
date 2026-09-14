# Device identity

Which device id this SDK computes, what it reads, what happens when a machine has no
hardware identity, and how to diagnose a mismatch.

[`FINGERPRINT_SPEC.md`](../FINGERPRINT_SPEC.md) is the normative, language-neutral,
cross-SDK algorithm and the definitive stability contract. This page is what that
spec means for *this* SDK; it never restates the contract, only links to it.

Every license is bound to the machine via a device id, stored in the token's `sig`
claim and re-checked on every local validation. The default
`moonbase_device_id_resolver` computes it from the cross-SDK
**[device fingerprint spec](../FINGERPRINT_SPEC.md)** (`moonbase:fingerprint:v2`): a
SHA-256 of stable native hardware identifiers, stamped with the spec version.

```
mbd2_9f3c…            // 'mbd' + version + '_' + 64 lowercase hex characters
```

Sources are SMBIOS on Windows, `IOPlatformUUID` via IOKit on macOS, and
`machine-id` plus world-readable DMI on Linux. No subprocess is spawned and no
root-only file is read, so the id is the same elevated or not, and the resolver
works inside an App Sandbox and a plugin host.

The algorithm is language-neutral by design: any Moonbase SDK that implements the
spec and passes the shipped
[`fingerprint-vectors.json`](../tests/vectors/fingerprint-vectors.json) computes the
same id on a given machine, so a license activated by one validates in the others.
Adoption is per-SDK: **this SDK conforms from 4.0.0; `@moonbase.sh/licensing`
conforms from 3.0.0.** Check the version of whichever SDK you are pairing with
before relying on it.

The id survives a rename, a locale change, a firmware update, a vCPU resize, and
running with or without elevated privileges. The spec's [**stability contract**](../FINGERPRINT_SPEC.md#stability-contract) is
the definitive list. Read it before shipping, along with the two Linux exceptions,
which exist because every per-unit hardware serial is root-only there and the id is
therefore tied to the OS installation rather than the hardware:

- A Linux **OS reinstall** requires re-activation.
- A Linux **VM cloned without clearing `/etc/machine-id`** keeps its device id, so
  a license copied with the disk keeps validating. `machine-id(5)` requires
  reusable images to ship that file empty; when they do, clones behave correctly.
  The SDK cannot detect a badly-prepared image, because the value that would
  distinguish the instances is root-only.

Because the version is part of the id, a mismatch is diagnosable. `validate_token`
throws `license_device_mismatch_error` either way, and the message says which case
you are in:

```cpp
try {
    licensing.validator().validate_token(token);
} catch (const moonbase::moonbase_error& ex) {
    if (ex.type() == moonbase::error_type::license_device_mismatch)
        std::cerr << ex.what(); // 'not for this device', plus any version difference
}
```

## When there is no hardware identity

The resolver throws `insufficient_device_identity_error` rather than falling back
to something weak, in two cases:

- **Nothing readable.** A locked-down process, or a platform with no defined
  parameters (Android, BSD).
- **Only model-level values readable.** Vendor, product and board names are
  byte-identical across every unit of a product line, so fingerprinting them would
  let those machines validate one another's licenses. In practice: a Linux install
  with no `machine-id`, or a machine whose SMBIOS carries an unset UUID *and* a
  blank or filler baseboard serial, the usual shape of a cloned VM image.

Opt in explicitly if a weaker id beats none. Those ids are stamped `mbd2n_` so the
server can tell them apart:

```cpp
moonbase::moonbase_device_id_resolver_options resolver_options;
resolver_options.fallback = moonbase::device_id_fallback::device_name;
auto resolver = std::make_shared<moonbase::moonbase_device_id_resolver>(resolver_options);
```

## Diagnostics and parity checks

`describe_device()` returns the id, spec version, platform tag and the *names* of
the parameters that contributed. It is safe to log or attach to a support ticket,
and returns a fresh copy each call so editing it cannot disturb the binding.

```cpp
if (const auto described = licensing.describe_device())
    std::cout << described->device_id << " (" << described->platform << ")\n";
```

Parameter values are never exposed there, and neither are per-parameter hashes.
They are hardware serial numbers, and an unsalted per-value digest is no safer to
publish than the value, since low-entropy values such as host names or sequential
serials fall to a dictionary. Which parameters contributed is the useful
diagnostic; what they read is not.

The device id itself is a one-way hash of all of them together, so it discloses no
individual serial. It is, however, **derived identically for every
Moonbase-powered product**. The material contains no product- or account-specific
input, so the same machine yields the same device id everywhere, and merchants
receive that string through the integration API and webhooks. Treat it as a stable
cross-vendor machine identifier. That is more than `machine-id(5)` intends, which
asks that the Linux machine id only leave the host through an
*application-specific keyed* derivation. If that matters for your deployment,
supply a custom `device_id_resolver` that mixes in a key of your own.

The lower-level `build_fingerprint_material`, `fingerprint_digest`,
`fingerprint_device_id` and `parse_device_id_stamp` helpers are exported from
`<moonbase/fingerprint_spec.hpp>` so you can verify cross-SDK parity against the
vector file. `examples/device_id.cpp` builds as the `moonbase_device_id` target and
prints all of the above as JSON, which is what
[the parity workflow](../.github/workflows/fingerprint-parity.yml) compares against
`@moonbase.sh/licensing` on every OS.
