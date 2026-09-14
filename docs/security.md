# Security and hardening

This SDK answers one question and answers it soundly: *is this license valid, for
this product, on this machine, right now?* What it cannot do is defend the code
that asks. Validation runs inside your binary, on a machine the user controls,
and the result eventually becomes a branch. Keeping a token honest and keeping a
shipped binary honest are two different problems; the SDK solves the first and
leaves the second to you, deliberately.

## What the SDK guarantees

- **Only Moonbase can mint a license.** Every token is an RS256 JWT verified
  against the public key you embed, with the algorithm pinned.
- **A license is bound to your product.** The `aud` claim must contain your
  `product_id`, and if you set `account_id` the issuer must match too.
- **A license is bound to one machine, and the binding is recomputed rather than
  read.** The `sig` claim is checked against a device id derived from the
  machine's own hardware on every validation, never from a file, so there is no
  stored value to edit. See [Device identity](device-identity.md).
- **Offline tolerance is bounded.** `online_validation_grace_period` caps how
  long an online-activated license runs without a successful server check.
  Transient failures fall back to the local result inside that window;
  definitive rejections propagate immediately regardless of it.
- **The stored file is a cache, not a credential.** The SDK re-derives every
  field from the signed token rather than trusting what it read. Do the same:
  branch on what the validator returned, never on fields read out of the store.
- **The defaults fail closed.** Nothing is persisted unless you supply a store,
  and the JUCE module's `LicenseGate` starts silent.

## What it deliberately leaves to you

There is no anti-debugging, obfuscation, integrity self-checking or tamper
detection anywhere in this SDK, and there will not be. Hardening works only when
it lives inside your binary and is specific to it: anything general enough to
ship in an open-source header would be public, identical in every plugin using
it, and one published bypass would apply to all of them. The SDK stops at the
boundary where it can still keep the promises it makes.

## Principles

- **Ask more than once, in more than one place.** A single call site that
  decides everything is a single thing to change.
- **Gate what the customer pays for**, not the window or the menu item.
- **Fail closed.** The failure you did not anticipate should be the safe one.
- **Separate detection from response.** Nothing requires a rejection to be
  immediate, adjacent to the check, or loud.
- **Take the free wins.** Release builds, stripped symbols, code signing.
- **Use the server you are already talking to.** Wire
  [revocation](core-sdk.md#revoking-an-activation) to a real "deactivate" affordance so seat
  limits mean something.
- **Price it honestly.** The goal is raising cost, not eliminating piracy, and
  every measure is paid for by legitimate users: the studio behind a locked-down
  network, the engineer who swapped a motherboard. Spending their goodwill to
  inconvenience people who were never going to buy is a poor trade.

For where this meets audio code, see the [integration paths](../README.md#pick-your-path)
and the `licensedFlag()` / `LicenseGate` helpers in
[`juce-module.md`](juce-module.md#gating).
