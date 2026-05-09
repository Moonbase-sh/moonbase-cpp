#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "moonbase/client.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint.hpp"
#include "moonbase/http.hpp"
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

    [[nodiscard]] license validate_token_online(std::string_view token) const
    {
        auto local = validator_->validate_token(token);

        if (local.method == activation_method::offline) {
            return local;
        }

        const auto age = std::chrono::system_clock::now() - local.validated_at;
        if (age < options_.online_validation_min_interval) {
            return local;
        }

        try {
            return client_->validate_token_online(token);
        } catch (const license_invalid_error&) {
            throw;
        } catch (const license_expired_error&) {
            throw;
        } catch (const std::exception&) {
            if (age <= options_.online_validation_grace_period) {
                return local;
            }
            throw;
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
};

} // namespace moonbase
