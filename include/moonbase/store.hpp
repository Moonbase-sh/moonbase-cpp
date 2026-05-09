#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "moonbase/errors.hpp"
#include "moonbase/types.hpp"

namespace moonbase {

class license_store {
public:
    virtual ~license_store() = default;
    [[nodiscard]] virtual std::optional<license> load_local_license() = 0;
    virtual void store_local_license(const license& value) = 0;
    virtual void delete_local_license() = 0;
};

class memory_license_store : public license_store {
public:
    [[nodiscard]] std::optional<license> load_local_license() override { return value_; }

    void store_local_license(const license& value) override { value_ = value; }

    void delete_local_license() override { value_.reset(); }

private:
    std::optional<license> value_;
};

class file_license_store : public license_store {
public:
    explicit file_license_store(
        std::filesystem::path path = std::filesystem::current_path() / "license.mb")
        : path_(std::move(path))
    {
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::optional<license> load_local_license() override
    {
        if (!std::filesystem::exists(path_)) {
            return std::nullopt;
        }

        std::ifstream file(path_);
        if (!file) {
            throw storage_error("Could not open local license file for reading");
        }

        try {
            nlohmann::json json;
            file >> json;
            return json.get<license>();
        } catch (const std::exception& ex) {
            throw storage_error(std::string("Could not parse local license file: ") + ex.what());
        }
    }

    void store_local_license(const license& value) override
    {
        const auto parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(path_, std::ios::trunc);
        if (!file) {
            throw storage_error("Could not open local license file for writing");
        }
        file << nlohmann::json(value).dump(2);
    }

    void delete_local_license() override
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (error) {
            throw storage_error("Could not delete local license file: " + error.message());
        }
    }

private:
    std::filesystem::path path_;
};

} // namespace moonbase
