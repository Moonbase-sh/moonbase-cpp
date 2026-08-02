#pragma once

// How this machine is identified to Moonbase.
//
// A license token carries a `sig` claim equal to the device id, recomputed and
// compared on every local validation. The default implementation
// (moonbase_device_id_resolver.hpp) follows the cross-SDK fingerprint spec, so a
// license activated by any conforming Moonbase SDK validates here and vice
// versa. Custom resolvers are compared literally and need not follow the spec.

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "moonbase/errors.hpp"
#include "moonbase/fingerprint_spec.hpp"

namespace moonbase {

/// A device id plus the provenance needed to reason about it: safe to log, show
/// in an about box, or attach to a support ticket.
///
/// Parameter *names* only, deliberately. Their values are hardware serial
/// numbers, and a hash of one is no safer to publish: an unsalted digest is a
/// stable global correlator for the machine, and low-entropy values such as host
/// names or sequential serials fall to a dictionary. machine-id(5) is explicit
/// that the Linux machine id is confidential and must only ever leave the host
/// through an application-specific *keyed* hash. Which parameters contributed is
/// the useful diagnostic anyway; what they read is not.
struct device_id_description {
    std::string device_id;
    /// Fingerprint spec version that produced it.
    int version = 0;
    std::string platform;
    fingerprint_spec::device_id_source source = fingerprint_spec::device_id_source::identity;
    /// Names of the identity parameters that went into the material, in order.
    std::vector<std::string> param_names;
};

class device_id_resolver {
public:
    virtual ~device_id_resolver() = default;

    /// A human-readable label sent alongside the device id at activation. Not
    /// part of the hashed material (except in the opt-in host-name fallback), so
    /// it can change freely without invalidating a license.
    [[nodiscard]] virtual std::string device_name() const = 0;

    /// The device id this machine binds on activation.
    [[nodiscard]] virtual std::string device_id() const = 0;

    /// Does this machine also answer to a device id it used to have?
    ///
    /// Deliberately separate from device_id(), which stays single-valued: what a
    /// device binds on activation and what a validator accepts are different
    /// questions, and conflating them is what forces an all-or-nothing migration.
    /// The default is no history, so a resolver that has never changed algorithm
    /// need not think about this.
    ///
    /// A virtual with a default rather than a second interface plus a
    /// dynamic_cast: plugin builds commonly disable RTTI, and the reference SDK
    /// duck-types this for the same reason.
    [[nodiscard]] virtual bool accepts_device_id(const std::string& /*device_id*/) const { return false; }

    /// Provenance for diagnostics, when the resolver can explain itself.
    [[nodiscard]] virtual std::optional<device_id_description> describe_device() const { return std::nullopt; }
};

/// A fixed identity. Useful in tests, and for apps that source the device id
/// from somewhere else entirely.
class static_device_id_resolver : public device_id_resolver {
public:
    static_device_id_resolver(std::string name, std::string id)
        : name_(std::move(name)), id_(std::move(id))
    {
    }

    [[nodiscard]] std::string device_name() const override { return name_; }
    [[nodiscard]] std::string device_id() const override { return id_; }

private:
    std::string name_;
    std::string id_;
};

/*
 * A note for anyone tempted to add a resolver that remembers a previously
 * computed device id on disk, to survive a transient read failure (a sandbox
 * refusing a firmware-table read, an unreadable /sys, a blocked IOKit call):
 *
 * It cannot be done safely at this layer. Any such cache is an unsigned file in
 * the application's own storage, so the attacker controls both its contents
 * *and* whether the fresh read is degraded. Recording the device id and
 * replaying it when the read looks weaker therefore reduces to "write the id you
 * want, then break one source": a scriptable license bypass, cheaper than
 * patching the binary.
 *
 * Corroborating the cache against the parameters that still read does not fix
 * it, because an attacker's own machine legitimately produces matching evidence,
 * so they pass the check while substituting any id they like.
 *
 * The only sound construction stores protected *inputs* and recomputes the id,
 * making a forged id require a SHA-256 preimage. That needs the raw parameter
 * values to derive a key from, which this layer deliberately never sees, since
 * it must not write hardware serials to disk. So it belongs inside the resolver
 * that reads them, if it is ever worth building.
 *
 * This SDK ships store.hpp, so an on-disk cache is a much shorter change here
 * than in the reference implementation. That makes the warning more important,
 * not less. The memoization in moonbase_device_id_resolver is process-lifetime
 * only. It is not persistence.
 */

/// Binds the current fingerprint while still recognising ids this device was
/// bound to before: the migration path off an older algorithm without a flag day.
///
/// device_id() always returns the *current* resolver's id, so every new
/// activation binds the current algorithm. The historical resolvers are consulted
/// only when a validator is deciding whether to accept an already-issued license.
/// A fleet therefore migrates as licenses are naturally re-activated, instead of
/// every device re-activating at once, which would burn a second activation seat
/// per device and reset device-scoped trials.
///
/// \code
/// auto resolver = std::make_shared<moonbase::migrating_device_id_resolver>(
///     std::make_shared<moonbase::moonbase_device_id_resolver>(),  // binds
///     std::make_shared<moonbase::legacy_cpp_device_id_resolver>()); // also accepted
/// \endcode
///
/// Historical ids are computed lazily, only on a mismatch, and then memoized, so
/// the happy path never pays for them. A historical resolver that throws is
/// skipped: it may simply not work on this platform any more, which just means it
/// cannot vouch for the license.
///
/// Every accepted id is recomputed from the machine's own hardware. Nothing is
/// read from disk, so widening what a validator accepts does not widen what an
/// attacker can assert.
class migrating_device_id_resolver : public device_id_resolver {
public:
    migrating_device_id_resolver(
        std::shared_ptr<device_id_resolver> current,
        std::vector<std::shared_ptr<device_id_resolver>> previous)
        : current_(std::move(current)), previous_(std::move(previous))
    {
        if (!current_) {
            throw configuration_error("A current device id resolver is required");
        }
    }

    template <
        typename... Previous,
        typename = std::enable_if_t<
            (std::is_convertible_v<Previous, std::shared_ptr<device_id_resolver>> && ...)>>
    explicit migrating_device_id_resolver(std::shared_ptr<device_id_resolver> current, Previous... previous)
        : migrating_device_id_resolver(
              std::move(current),
              std::vector<std::shared_ptr<device_id_resolver>>{std::move(previous)...})
    {
    }

    [[nodiscard]] std::string device_name() const override { return current_->device_name(); }
    [[nodiscard]] std::string device_id() const override { return current_->device_id(); }

    [[nodiscard]] bool accepts_device_id(const std::string& device_id) const override
    {
        // An empty id is what a historical resolver that could not read anything
        // reduces to. It must never match, or a machine with no identity would
        // accept a license bound to another such machine.
        if (device_id.empty()) {
            return false;
        }

        std::call_once(previous_ids_once_, [this] {
            previous_ids_.reserve(previous_.size());
            for (const auto& resolver : previous_) {
                if (!resolver) {
                    continue;
                }
                try {
                    previous_ids_.push_back(resolver->device_id());
                } catch (...) {
                    // Cannot vouch for the license on this machine; carry on.
                }
            }
        });

        return std::any_of(
            previous_ids_.begin(), previous_ids_.end(), [&device_id](const std::string& previous) {
                return !previous.empty() && previous == device_id;
            });
    }

    /// Forwarded so the current resolver stays describable through this wrapper.
    [[nodiscard]] std::optional<device_id_description> describe_device() const override
    {
        return current_->describe_device();
    }

private:
    std::shared_ptr<device_id_resolver> current_;
    std::vector<std::shared_ptr<device_id_resolver>> previous_;

    mutable std::once_flag previous_ids_once_;
    mutable std::vector<std::string> previous_ids_;
};

} // namespace moonbase
