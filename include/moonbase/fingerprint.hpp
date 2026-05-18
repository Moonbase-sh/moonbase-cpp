#pragma once

#include <string>
#include <utility>

namespace moonbase {

class fingerprint_provider {
public:
    virtual ~fingerprint_provider() = default;
    [[nodiscard]] virtual std::string device_name() const = 0;
    [[nodiscard]] virtual std::string device_id() const = 0;
};

class static_fingerprint_provider : public fingerprint_provider {
public:
    static_fingerprint_provider(std::string name, std::string id)
        : name_(std::move(name)), id_(std::move(id))
    {
    }

    [[nodiscard]] std::string device_name() const override { return name_; }
    [[nodiscard]] std::string device_id() const override { return id_; }

private:
    std::string name_;
    std::string id_;
};

} // namespace moonbase
