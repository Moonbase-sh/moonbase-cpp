#pragma once

#include <stdexcept>
#include <string>

namespace moonbase {

enum class error_type {
    api_error,
    license_invalid,
    license_expired,
    storage_error,
    configuration_error,
    operation_not_supported,
};

class moonbase_error : public std::runtime_error {
public:
    moonbase_error(error_type type, std::string message)
        : std::runtime_error(std::move(message)), type_(type)
    {
    }

    [[nodiscard]] error_type type() const noexcept { return type_; }

private:
    error_type type_;
};

class configuration_error : public moonbase_error {
public:
    explicit configuration_error(const std::string& message)
        : moonbase_error(error_type::configuration_error, message)
    {
    }
};

class api_error : public moonbase_error {
public:
    api_error(int status_code, std::string message, std::string title = {}, std::string detail = {})
        : moonbase_error(error_type::api_error, std::move(message)),
          status_code_(status_code),
          title_(std::move(title)),
          detail_(std::move(detail))
    {
    }

    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    int status_code_;
    std::string title_;
    std::string detail_;
};

class license_invalid_error : public moonbase_error {
public:
    explicit license_invalid_error(const std::string& message)
        : moonbase_error(error_type::license_invalid, message)
    {
    }
};

class license_expired_error : public moonbase_error {
public:
    explicit license_expired_error(const std::string& message)
        : moonbase_error(error_type::license_expired, message)
    {
    }
};

class storage_error : public moonbase_error {
public:
    explicit storage_error(const std::string& message)
        : moonbase_error(error_type::storage_error, message)
    {
    }
};

class operation_not_supported_error : public moonbase_error {
public:
    explicit operation_not_supported_error(const std::string& message)
        : moonbase_error(error_type::operation_not_supported, message)
    {
    }
};

} // namespace moonbase
