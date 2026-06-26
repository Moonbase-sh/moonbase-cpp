#pragma once

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "moonbase/detail/file_lock.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/types.hpp"

namespace moonbase {

// Opaque RAII handle returned by license_store::lock_for_update(). Holding it
// guarantees exclusive access to the underlying store for the lifetime of the
// handle, including across processes when the store is backed by a shared
// resource (e.g. a file).
class store_lock_guard {
public:
    virtual ~store_lock_guard() = default;
};

class license_store {
public:
    virtual ~license_store() = default;
    [[nodiscard]] virtual std::optional<license> load_local_license() = 0;
    virtual void store_local_license(const license& value) = 0;
    virtual void delete_local_license() = 0;

    // Acquires an exclusive lock spanning the load/validate/store critical
    // section in licensing::validate_token_online. Returns nullptr if the
    // store does not require coordination (e.g. an in-memory store used by a
    // single SDK instance); the SDK then relies on its in-process mutex only.
    [[nodiscard]] virtual std::unique_ptr<store_lock_guard> lock_for_update()
    {
        return nullptr;
    }
};

class memory_license_store : public license_store {
public:
    [[nodiscard]] std::optional<license> load_local_license() override
    {
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        return value_;
    }

    void store_local_license(const license& value) override
    {
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        value_ = value;
    }

    void delete_local_license() override
    {
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        value_.reset();
    }

    // In-process serialization for the validate→persist critical section.
    // Without this, a concurrent clearLicense() running on another thread
    // (the SDK's validate_mutex_ only serializes validate calls, not external
    // mutations of the store) could delete between should_persist returning
    // true and the actual store_local_license call, resurrecting the cleared
    // license in memory.
    //
    // Recursive so callers (notably validate_token_online) can keep using
    // load_local_license / store_local_license on the same thread while
    // holding the guard.
    [[nodiscard]] std::unique_ptr<store_lock_guard> lock_for_update() override
    {
        return std::make_unique<memory_store_lock>(mutex_);
    }

private:
    class memory_store_lock : public store_lock_guard {
    public:
        explicit memory_store_lock(std::recursive_mutex& mutex) : guard_(mutex) {}

    private:
        std::lock_guard<std::recursive_mutex> guard_;
    };

    std::recursive_mutex mutex_;
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

        errno = 0;
        std::ifstream file(path_);
        if (!file) {
            const int err = errno;
            throw storage_error("Could not open local license file for reading: " + path_.string()
                                + errno_suffix(err));
        }

        try {
            nlohmann::json json;
            file >> json;
            return json.get<license>();
        } catch (const std::exception&) {
            // A corrupt / unparseable license file is useless and would otherwise
            // throw on every load, blocking re-activation. Remove it (best-effort,
            // after closing the handle so Windows can unlink it) and report no
            // stored license, so a fresh activation can be written in its place.
            file.close();
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            return std::nullopt;
        }
    }

    void store_local_license(const license& value) override
    {
        const auto parent = path_.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                throw storage_error("Could not create license directory " + parent.string()
                                    + ": " + ec.message());
            }
        }

        errno = 0;
        std::ofstream file(path_, std::ios::trunc);
        if (!file) {
            const int err = errno;
            throw storage_error("Could not open local license file for writing: " + path_.string()
                                + errno_suffix(err));
        }
        file << nlohmann::json(value).dump(2);
        file.flush();
        if (!file) {
            throw storage_error("Could not write local license file: " + path_.string());
        }
    }

    void delete_local_license() override
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (error) {
            throw storage_error("Could not delete local license file " + path_.string() + ": "
                                + error.message());
        }
    }

    [[nodiscard]] std::unique_ptr<store_lock_guard> lock_for_update() override
    {
        // Lock a sidecar file, not the license file itself. delete_local_license
        // unlinks the license path, which on POSIX would orphan an flock on
        // that inode: a sibling instance could then open a fresh inode at the
        // same path and acquire an independent lock, defeating the
        // serialization. The sidecar is created on first use and never
        // deleted by us, so its inode is stable for the lifetime of the
        // process tree.
        auto lock_path = path_;
        lock_path += ".lock";
        return std::make_unique<file_store_lock>(lock_path);
    }

private:
    // Best-effort OS reason for a failed std::fstream open. Stream failures don't
    // portably set errno, so callers reset errno to 0 first and we only append a
    // reason when something was actually recorded.
    static std::string errno_suffix(int err)
    {
        if (err == 0) {
            return {};
        }
        return " (" + std::generic_category().message(err) + ")";
    }

    class file_store_lock : public store_lock_guard {
    public:
        explicit file_store_lock(const std::filesystem::path& path) : lock_(path) {}

    private:
        detail::file_lock lock_;
    };

    std::filesystem::path path_;
};

} // namespace moonbase
