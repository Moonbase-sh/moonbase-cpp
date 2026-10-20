#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "moonbase/client.hpp"
// Before 4.0.0 this header pulled in fingerprint.hpp and default_fingerprint.hpp,
// so a consumer including only <moonbase/licensing.hpp> could name
// fingerprint_provider, static_fingerprint_provider and
// default_fingerprint_provider. Those aliases are deprecated, not gone, so keep
// providing them here until they are removed in 5.0.0: dropping the includes would
// turn a documented deprecation warning into a hard compile error for exactly the
// consumers the aliases exist to protect.
#include "moonbase/default_fingerprint.hpp"
#include "moonbase/detail/base64.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint.hpp"
#include "moonbase/http.hpp"
#ifndef MOONBASE_DISABLE_CURL_TRANSPORT
#include "moonbase/http_curl.hpp"
#endif
#include "moonbase/moonbase_device_id_resolver.hpp"
#include "moonbase/release_access.hpp"
#include "moonbase/store.hpp"
#include "moonbase/types.hpp"
#include "moonbase/validator.hpp"

namespace moonbase {

class licensing {
public:
    explicit licensing(
        licensing_options options,
        std::shared_ptr<license_store> store = nullptr,
        std::shared_ptr<device_id_resolver> device_ids = nullptr,
        std::shared_ptr<http_transport> transport = nullptr)
        : options_(normalize_and_validate(std::move(options))),
          store_(std::move(store)),
          device_ids_(std::move(device_ids)),
          transport_(std::move(transport))
    {
        if (!store_) {
            store_ = std::make_shared<memory_license_store>();
        }
        if (!device_ids_) {
            device_ids_ = std::make_shared<moonbase_device_id_resolver>();
        }
        if (!transport_) {
#ifdef MOONBASE_DISABLE_CURL_TRANSPORT
            throw configuration_error(
                "An HTTP transport is required (built without the bundled CURL transport)");
#else
            transport_ = std::make_shared<curl_http_transport>();
#endif
        }
        validator_ = std::make_shared<license_validator>(options_, device_ids_);
        client_ = std::make_shared<license_client>(options_, device_ids_, validator_, transport_);
    }

    // Starts a browser activation. Poll the returned request with
    // get_requested_activation every 2 to 3 seconds until it yields a license.
    // The SDK sends at most one poll per second for each request; calls in
    // between return nullopt. A request that expires or is cancelled makes the
    // poll throw activation_request_error: stop, and start a new one.
    //
    // method (optional): pass activation_method::offline to ask the backend for
    // an offline license. The browser flow itself is identical, but the token it
    // mints carries method: Offline, so it is validated locally for good
    // (validate_token_online short-circuits it) and cannot be revoked. The
    // product must have offline activations enabled, otherwise the call throws
    // license_invalid_error reading "Product does not allow offline activations".
    //
    // This is the second route to an offline license, alongside the file-based
    // generate_device_token / read_offline_license exchange. Use this one when
    // the machine has network at activation time but not afterwards; use the
    // file exchange when it has no network at all.
    [[nodiscard]] activation_request request_activation(
        activation_method method = activation_method::online) const
    {
        return client_->request_activation(method);
    }

    [[nodiscard]] std::optional<license> get_requested_activation(
        const activation_request& activation) const
    {
        return client_->get_requested_activation(activation);
    }

    // Signature, device binding, expiry and, for a license that covers only some
    // versions, application_version (release_not_covered_error), with no network.
    [[nodiscard]] license validate_token_local(std::string_view token) const
    {
        return validator_->validate_token(token);
    }

    // Same as validate_token_local but does not throw on a past `exp`, nor for a
    // version the license doesn't cover. Lets a caller inspect an aged-out token
    // (e.g. to learn its activation method and expiry) and decide what to do
    // with it, rather than only seeing the throw.
    [[nodiscard]] license validate_token_local_allow_expired(std::string_view token) const
    {
        return validator_->validate_token_allow_expired(token);
    }

    // Emits the device token ("machine file") for the offline activation flow.
    // The returned string is a base64-encoded JSON descriptor of this device +
    // product; write it to a file (conventionally with a ".dt" extension) that
    // the customer uploads at https://<tenant>.moonbase.sh/activate to obtain an
    // offline-activated license token in return.
    [[nodiscard]] std::string generate_device_token() const
    {
        // As request_activation does: a name that is not UTF-8 is repaired, since
        // it is only a label, and an id that is not UTF-8 is refused.
        const auto device_id = device_ids_->device_id();
        detail::require_utf8_device_id(device_id);
        const nlohmann::json payload{
            {"id", device_id},
            {"name", detail::replace_invalid_utf8(device_ids_->device_name())},
            {"productId", options_.product_id},
            // The Moonbase API expects this to always be "JWT".
            {"format", "JWT"},
        };
        const auto json = payload.dump();
        return detail::base64_encode(
            reinterpret_cast<const unsigned char*>(json.data()), json.size());
    }

    // Reads back the offline-activated license token the customer downloaded
    // from the portal (the raw JWT contents of the license file). Validates it
    // locally (signature, audience, issuer, device fingerprint, expiry and
    // application_version) and requires the token to have been issued via
    // offline activation.
    // Offline tokens are never re-validated against the API, so there is no
    // online counterpart. Persist the result with
    // store().store_local_license(...), mirroring the online flow.
    [[nodiscard]] license read_offline_license(std::string_view token) const
    {
        // The version is judged last, so release_not_covered_error only ever
        // carries an offline license for this device, safe to store.
        auto local = validator_->validate_token_ignoring_release_access(token);
        if (local.method != activation_method::offline) {
            throw license_invalid_error(
                "License token is not an offline-activated license");
        }
        validator_->ensure_covers_application_version(local);
        return local;
    }

    // should_persist (optional): evaluated inside the SDK's locks immediately
    // before the refreshed license would be written to the store. When it
    // returns false, the SDK skips the persist but still returns the
    // refreshed license. Lets callers cancel writes that have been invalidated
    // by concurrent state changes (e.g. the user pressed "Deactivate" while a
    // background revalidation was in flight) without dropping the file lock
    // in between — the predicate runs while the lock is still held, so a
    // racing clearLicense() either bumped its flag before the predicate read
    // (we skip), or after (we persist and the clear's own delete-under-lock
    // runs strictly after our release).
    //
    // A license that doesn't cover application_version throws
    // release_not_covered_error. Moonbase judges that for an online license,
    // since it knows of an upgrade bought since the token was issued, which the
    // device cannot; the device judges it only when Moonbase isn't asked (an
    // offline license, the min_interval throttle, the grace period). When
    // Moonbase refused, the fresh copy it sent along is persisted first (subject
    // to should_persist), as a refreshed license is: it still covers the
    // versions it did.
    [[nodiscard]] license validate_token_online(
        std::string_view token,
        std::function<bool()> should_persist = {}) const
    {
        auto local = validator_->validate_token_ignoring_release_access(token);

        if (local.method == activation_method::offline) {
            validator_->ensure_covers_application_version(local);
            return local;
        }

        // Cross-instance deduplication: many DAW plugin instances frequently
        // call this concurrently (project load with N plugin instances; in
        // sandboxed hosts each instance is its own process). Layered guards:
        //   1. Process-local mutex: serializes same-process callers cheaply.
        //   2. File lock from the store: serializes across processes that
        //      share the license file.
        //   3. Reload-under-lock + persist-on-success: the second waiter
        //      observes the fresh validated_at the first writer just wrote
        //      and short-circuits via the throttle.
        std::lock_guard<std::mutex> in_process_guard(validate_mutex_);
        auto cross_process_guard = store_->lock_for_update();

        license freshest = local;
        // Two copies validated at the same time can still differ: an add-on call
        // answers with the activation's last validation time rather than a new
        // one. Nothing in them says which is current, so the server is asked.
        bool undecided = false;
        try {
            if (auto stored = store_->load_local_license();
                stored && stored->activation_id == local.activation_id) {
                auto candidate = validator_->validate_token_ignoring_release_access(stored->token);
                if (candidate.validated_at > freshest.validated_at) {
                    freshest = std::move(candidate);
                } else if (candidate.validated_at == freshest.validated_at && candidate.token != freshest.token) {
                    undecided = true;
                }
            }
        } catch (const std::exception&) {
            // A corrupt or unparseable store entry must not block validation;
            // fall back to the caller-supplied token.
        }

        const auto age = std::chrono::system_clock::now() - freshest.validated_at;

        // The throttle is only allowed to skip the API while we're also
        // inside the grace period — otherwise a min_interval longer than the
        // grace period would silently extend "max age without an online
        // check" past its advertised limit.
        if (!undecided && age < options_.online_validation_min_interval
            && age <= options_.online_validation_grace_period) {
            validator_->ensure_covers_application_version(freshest);
            return freshest;
        }

        try {
            auto refreshed = client_->validate_token_online(token);
            // Persist so sibling processes/instances see the fresh
            // validated_at and hit the throttle on their next call. Storage
            // failures are best-effort — we already have a valid license to
            // return.
            if (!should_persist || should_persist()) {
                try {
                    store_->store_local_license(refreshed);
                } catch (const storage_error&) {
                }
            }
            return refreshed;
        } catch (const release_not_covered_error& ex) {
            if (ex.license() && (!should_persist || should_persist())) {
                try {
                    store_->store_local_license(*ex.license());
                } catch (const storage_error&) {
                }
            }
            throw;
        } catch (const license_invalid_error&) {
            throw;
        } catch (const license_expired_error&) {
            throw;
        } catch (const std::exception&) {
            // store_closed_error included: it is not signed, so it relaxes nothing.
            if (age <= options_.online_validation_grace_period) {
                validator_->ensure_covers_application_version(freshest);
                return freshest;
            }
            throw;
        }
    }

    // Takes a seat of one of the product's add-ons for this device, from the
    // license the device holds for the product, and returns that license again,
    // reporting the add-on in sub_products. The add-on must be included in the
    // product's license. sub_product_id is the add-on's own part of its product
    // ID ("drum-kit") or its whole ID ("my-app.drum-kit").
    //
    // For apps that take add-on seats themselves (auto_activate_sub_products
    // off), and for an add-on whose seat on this device was revoked (revoked),
    // which validating does not take back.
    //
    // The returned license is stored, as validate_token_online stores its own:
    // under the same locks, held from before the request until the write, so
    // another call here or in another instance can't store an older answer over
    // it. should_persist, when given, is asked under those locks just before the
    // write, and false skips it.
    //
    // Throws operation_not_supported_error for a trial license without contacting
    // the API, no_eligible_license_error when the customer has no seat of the
    // add-on to give (they don't own it, or every seat is taken), and
    // operation_not_supported_error when the server refuses the add-on for this
    // license. license_invalid_error or license_expired_error mean the license
    // itself is no longer good. Other failures are api_error and worth retrying.
    [[nodiscard]] license activate_sub_product(
        std::string_view token,
        std::string_view sub_product_id,
        std::function<bool()> should_persist = {}) const
    {
        require_license_for_sub_products(token);
        return call_and_store([&] { return client_->activate_sub_product(token, sub_product_id); },
                              should_persist);
    }

    // Starts the trial of one of the product's add-ons on this device, from the
    // license the device holds for the product, and returns that license again,
    // reporting the trial in sub_products. The add-on must be included in the
    // product's license and offer trials, and the license must be an online
    // license rather than a trial. A customer who owns the add-on gets a seat of
    // it instead, as activate_sub_product does. A device gets one trial of each
    // add-on: asking again returns the license with the trial it had, expired or
    // not.
    //
    // Stores the result and throws as activate_sub_product does.
    [[nodiscard]] license request_sub_product_trial(
        std::string_view token,
        std::string_view sub_product_id,
        std::function<bool()> should_persist = {}) const
    {
        require_license_for_sub_products(token);
        return call_and_store([&] { return client_->request_sub_product_trial(token, sub_product_id); },
                              should_persist);
    }

    // Lists the product's add-ons for this device, from the license or trial the
    // device holds for the product: each with its prices when it is for sale,
    // whether the customer owns it, whether to unlock it here, and its trial,
    // including whether request_sub_product_trial can start one now. Changes
    // nothing, so it can back a store or upgrade screen.
    //
    // Checks the token locally first. Throws as activate_sub_product does, apart
    // from the local trial check, and api_error for an answer that isn't the list.
    [[nodiscard]] sub_product_catalog list_sub_products(std::string_view token) const
    {
        // Not against the app's version: add-ons are managed on the license
        // whatever version runs, as revoking is.
        static_cast<void>(validator_->validate_token_ignoring_release_access(token));
        return client_->list_sub_products(token);
    }

    void revoke_activation(std::string_view token) const
    {
        // allow_expired: a long-running app may click "Deactivate" after the
        // local token has aged out, but the server-side seat is still
        // allocated until we POST to /revoke.
        const auto local = validator_->validate_token_allow_expired(token);

        if (local.method == activation_method::offline) {
            throw operation_not_supported_error(
                "Offline-activated licenses cannot be revoked");
        }
        if (local.trial) {
            throw operation_not_supported_error(
                "Trial licenses cannot be revoked");
        }

        client_->revoke_activation(token);

        // Store cleanup is best-effort — the server-side seat is already
        // freed, so a local IO failure must not surface as a revoke failure
        // (callers would retry against a token the server no longer knows).
        //
        // Hold the cross-process update lock for the load/delete window so an
        // in-flight validate_token_online in a sibling instance can't slip a
        // persist between our read and our delete (which would resurrect the
        // license the user just revoked).
        try {
            auto cross_process_guard = store_->lock_for_update();
            if (auto stored = store_->load_local_license();
                stored && stored->activation_id == local.activation_id) {
                store_->delete_local_license();
            }
        } catch (const storage_error&) {
        }
    }

    [[nodiscard]] license_store& store() noexcept { return *store_; }
    [[nodiscard]] const license_store& store() const noexcept { return *store_; }

    [[nodiscard]] license_client& client() noexcept { return *client_; }
    [[nodiscard]] const license_client& client() const noexcept { return *client_; }

    [[nodiscard]] license_validator& validator() noexcept { return *validator_; }
    [[nodiscard]] const license_validator& validator() const noexcept { return *validator_; }

    [[nodiscard]] moonbase::device_id_resolver& device_resolver() noexcept { return *device_ids_; }
    [[nodiscard]] const moonbase::device_id_resolver& device_resolver() const noexcept { return *device_ids_; }

    // How this machine's device id was derived, for diagnostics. Names of the
    // contributing identity parameters only, never their values. Empty when the
    // resolver does not describe itself, as a custom one need not.
    //
    // Throws insufficient_device_identity_error when this machine has no readable
    // identity, the same as device_resolver().device_id() would.
    [[nodiscard]] std::optional<device_id_description> describe_device() const
    {
        return device_ids_->describe_device();
    }

#if !defined(MOONBASE_DISABLE_DEPRECATED_ALIASES)
    [[deprecated("renamed to device_resolver()")]] [[nodiscard]] moonbase::device_id_resolver&
    fingerprint() noexcept
    {
        return *device_ids_;
    }

    [[deprecated("renamed to device_resolver()")]] [[nodiscard]] const moonbase::device_id_resolver&
    fingerprint() const noexcept
    {
        return *device_ids_;
    }
#endif

private:
    // Add-ons are activated and tried from a license: the server refuses a trial,
    // so this saves the round trip and says so plainly.
    void require_license_for_sub_products(std::string_view token) const
    {
        if (validator_->validate_token_ignoring_release_access(token).trial) {
            throw operation_not_supported_error(
                "Add-ons are activated and tried from a license, not from a trial");
        }
    }

    // Makes an add-on call and stores the license it returns, holding the locks
    // validate_token_online holds from before the request until the write. The
    // server marks an add-on call's answer with the activation's last validation
    // time, not a new one, so the order answers reach the store in is the only
    // order they have: holding the locks makes that the order the server gave
    // them in. Storing is best-effort, as there: the license is good whether or
    // not it reached the store.
    template <typename Call>
    license call_and_store(Call&& call, const std::function<bool()>& should_persist) const
    {
        std::lock_guard<std::mutex> in_process_guard(validate_mutex_);
        auto cross_process_guard = store_->lock_for_update();

        auto value = call();
        if (!should_persist || should_persist()) {
            try {
                store_->store_local_license(value);
            } catch (const storage_error&) {
            }
        }
        return value;
    }

    static licensing_options normalize_and_validate(licensing_options options)
    {
        options.endpoint = detail::trim_trailing_slashes(options.endpoint);
        if (options.endpoint.empty()) {
            throw configuration_error("Moonbase endpoint is required");
        }
        if (options.product_id.empty()) {
            throw configuration_error("Moonbase product_id is required");
        }
        if (options.public_key.empty()) {
            throw configuration_error("Moonbase public_key is required");
        }
        return options;
    }

    licensing_options options_;
    std::shared_ptr<license_store> store_;
    std::shared_ptr<device_id_resolver> device_ids_;
    std::shared_ptr<http_transport> transport_;
    std::shared_ptr<license_validator> validator_;
    std::shared_ptr<license_client> client_;
    mutable std::mutex validate_mutex_;
};

} // namespace moonbase
