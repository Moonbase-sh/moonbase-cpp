#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <moonbase/moonbase.hpp>

namespace {

std::string require_env(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing required environment variable: ") + name);
    }
    return value;
}

std::string optional_env(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

} // namespace

int main()
{
    try {
        moonbase::licensing_options options;
        options.endpoint = require_env("MOONBASE_ENDPOINT");
        options.product_id = require_env("MOONBASE_PRODUCT_ID");
        options.public_key = require_env("MOONBASE_PUBLIC_KEY");
        const auto app_version = optional_env("MOONBASE_APP_VERSION");
        if (!app_version.empty()) {
            options.application_version = app_version;
        }
        const auto account_id = optional_env("MOONBASE_ACCOUNT_ID");
        if (!account_id.empty()) {
            options.account_id = account_id;
        }

        auto store = std::make_shared<moonbase::file_license_store>();
        moonbase::licensing licensing(options, store);

        if (auto existing = licensing.store().load_local_license()) {
            const auto validated = licensing.validate_token_local(existing->token);
            std::cout << "Existing license is valid for "
                      << validated.issued_to.email << '\n';
            return 0;
        }

        const auto request = licensing.request_activation();
        std::cout << "Open this URL to activate:\n"
                  << request.browser_url << "\n\n";

        std::optional<moonbase::license> activated;
        while (!activated) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            activated = licensing.get_requested_activation(request);
            std::cout << "Waiting for activation...\n";
        }

        licensing.store().store_local_license(*activated);
        std::cout << "Stored license for " << activated->issued_to.email << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
