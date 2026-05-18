#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "moonbase/client.hpp"
#include "moonbase/default_fingerprint.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint.hpp"
#include "moonbase/http.hpp"
#include "moonbase/http_curl.hpp"
#include "moonbase/store.hpp"
#include "moonbase/types.hpp"
#include "moonbase/validator.hpp"

namespace moonbase {

class licensing {
public:
    explicit licensing(
        licensing_options options,
        std::shared_ptr<license_store> store = nullptr,
        std::shared_ptr<fingerprint_provider> fingerprints = nullptr,
        std::shared_ptr<http_transport> transport = nullptr)
        : options_(normalize_and_validate(std::move(options))),
          store_(std::move(store)),
          fingerprints_(std::move(fingerprints)),
          transport_(std::move(transport))
    {
        if (!store_) {
            store_ = std::make_shared<memory_license_store>();
        }
        if (!fingerprints_) {
            fingerprints_ = std::make_shared<default_fingerprint_provider>();
        }
        if (!transport_) {
            transport_ = std::make_shared<curl_http_transport>();
        }
        validator_ = std::make_shared<license_validator>(options_, fingerprints_);
        client_ = std::make_shared<license_client>(options_, fingerprints_, validator_, transport_);
    }

    [[nodiscard]] activation_request request_activation() const
    {
        return client_->request_activation();
    }

    [[nodiscard]] std::optional<license> get_requested_activation(
        const activation_request& activation) const
    {
        return client_->get_requested_activation(activation);
    }

    [[nodiscard]] license validate_token_local(std::string_view token) const
    {
        return validator_->validate_token(token);
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
    [[nodiscard]] license validate_token_online(
        std::string_view token,
        std::function<bool()> should_persist = {}) const
    {
        auto local = validator_->validate_token(token);

        if (local.method == activation_method::offline) {
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
        try {
            if (auto stored = store_->load_local_license();
                stored && stored->activation_id == local.activation_id) {
                auto candidate = validator_->validate_token(stored->token);
                if (candidate.validated_at > freshest.validated_at) {
                    freshest = std::move(candidate);
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
        if (age < options_.online_validation_min_interval
            && age <= options_.online_validation_grace_period) {
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
        } catch (const license_invalid_error&) {
            throw;
        } catch (const license_expired_error&) {
            throw;
        } catch (const std::exception&) {
            if (age <= options_.online_validation_grace_period) {
                return freshest;
            }
            throw;
        }
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

    [[nodiscard]] moonbase::fingerprint_provider& fingerprint() noexcept { return *fingerprints_; }
    [[nodiscard]] const moonbase::fingerprint_provider& fingerprint() const noexcept { return *fingerprints_; }

private:
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
    std::shared_ptr<fingerprint_provider> fingerprints_;
    std::shared_ptr<http_transport> transport_;
    std::shared_ptr<license_validator> validator_;
    std::shared_ptr<license_client> client_;
    mutable std::mutex validate_mutex_;
};

} // namespace moonbase
