#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

        std::ifstream file;
        int err = 0;
        const bool opened = retry_while_in_use([&] {
            errno = 0;
            file.open(path_);
            err = errno;
            return file.is_open();
        });
        if (!opened) {
            throw storage_error("Could not open local license file for reading: " + utf8_path(path_)
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
                throw storage_error("Could not create license directory " + utf8_path(parent)
                                    + ": " + ec.message());
            }
        }

        const auto contents = nlohmann::json(value).dump(2);

        // Write the new license beside the old one and rename it into place, so
        // a reader sees one or the other and never a half-written file, which
        // load_local_license would delete as corrupt. That holds for readers
        // that don't take the lock, such as an instance loading at startup.
        if (replace_with_temp_file(contents)) {
            return;
        }

        // Where that fails, as in a folder that takes no new files but lets the
        // license itself be rewritten, write it in place.
        errno = 0;
        std::ofstream file(path_, std::ios::trunc);
        if (!file) {
            const int err = errno;
            throw storage_error("Could not open local license file for writing: " + utf8_path(path_)
                                + errno_suffix(err));
        }
        file << contents;
        file.flush();
        if (!file) {
            throw storage_error("Could not write local license file: " + utf8_path(path_));
        }
    }

    void delete_local_license() override
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (error) {
            throw storage_error("Could not delete local license file " + utf8_path(path_) + ": "
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
        return std::make_unique<file_store_lock>(lock_path, *lock_waits_cancelled_);
    }

    // For shutting down: from here on lock_for_update() throws storage_error
    // instead of waiting for a lock that another instance or process holds, and
    // a call already waiting gives up the same way. A free lock is still taken.
    // Without this, an instance closing while a sibling re-validates waits for
    // that check, which offline can run until its request times out. Callable
    // from any thread; it cannot be undone.
    void cancel_lock_waits() noexcept
    {
        lock_waits_cancelled_->store(true, std::memory_order_release);
    }

private:
    // Writes contents to a new file beside the license and renames it over the
    // license. False, with the new file removed again, if any step fails or the
    // rename would change more than the contents.
    bool replace_with_temp_file(const std::string& contents) const
    {
        // Follow a symlinked license path to the file itself, so the link keeps
        // pointing at the license instead of being replaced by a copy of it.
        auto target = path_;
        std::error_code link_error;
        if (std::filesystem::is_symlink(path_, link_error)) {
            target = std::filesystem::canonical(path_, link_error);
            if (link_error) {
                return false; // a dangling link: writing in place creates its target
            }
        }

        // No license yet is fine (status reports that as an error too). One also
        // known by another name, a hard link, would keep the old contents there.
        std::error_code status_error;
        const auto existing = std::filesystem::status(target, status_error);
        const bool exists = std::filesystem::exists(existing);
        std::error_code links_error;
        if (exists && std::filesystem::hard_link_count(target, links_error) != 1) {
            return false;
        }

        auto temp = target;
        temp += temp_suffix();
        {
            std::ofstream file(temp, std::ios::trunc);
            if (!file) {
                return false;
            }
            // Keep the license's permissions, so one restricted to its owner
            // stays that way, and set them before the contents land. Only on
            // POSIX: on Windows these are just the read-only flag.
            std::error_code mode_error;
#if !defined(_WIN32)
            if (exists) {
                std::filesystem::permissions(temp, existing.permissions(), mode_error);
            }
#endif
            if (!mode_error) {
                file << contents;
            }
            file.close();
            if (mode_error || !file) {
                std::error_code ignored;
                std::filesystem::remove(temp, ignored);
                return false;
            }
        }

        std::error_code ec;
        if (retry_while_in_use([&] {
                std::filesystem::rename(temp, target, ec);
                return !ec;
            })) {
            return true;
        }
        std::filesystem::remove(temp, ec);
        return false;
    }

    // Windows won't open or replace a file while another handle holds it in a
    // conflicting way. A sibling reading the license holds it like that for a
    // moment, so does a rename that just put a new license in place, and a virus
    // scanner may for longer. So there, a failed attempt is tried a few more
    // times before giving up; elsewhere it runs once.
    template <typename Attempt>
    static bool retry_while_in_use(Attempt&& attempt)
    {
#if defined(_WIN32)
        constexpr int attempts = 10;
#else
        constexpr int attempts = 1;
#endif
        auto pause = std::chrono::milliseconds(1);
        for (int tried = 1;; ++tried) {
            if (attempt()) {
                return true;
            }
            if (tried == attempts) {
                return false;
            }
            std::this_thread::sleep_for(pause);
            pause = std::min(pause * 2, std::chrono::milliseconds(100));
        }
    }

    // Unique to one write, so writers never share a temporary file, whether in
    // different processes, on different threads, or in two plugin binaries
    // loaded into the same host.
    static std::string temp_suffix()
    {
#if defined(_WIN32)
        const auto process = static_cast<long long>(::_getpid());
#else
        const auto process = static_cast<long long>(::getpid());
#endif
        static std::atomic<unsigned long long> sequence{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return "." + std::to_string(process) + "-" + std::to_string(ticks) + "-"
            + std::to_string(sequence.fetch_add(1)) + ".tmp";
    }

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

    // The path as UTF-8, for messages. path::string() converts to the ANSI code
    // page on Windows, where MSVC throws for a letter with no mapping there (a
    // Japanese user folder on a Western-locale machine), and that would replace
    // the storage_error being built with an unrelated conversion error.
    static std::string utf8_path(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string(); // std::string before C++20, std::u8string from it
        return std::string(utf8.begin(), utf8.end());
    }

    class file_store_lock : public store_lock_guard {
    public:
        file_store_lock(const std::filesystem::path& path, const std::atomic<bool>& give_up)
            : lock_(path, give_up)
        {
        }

    private:
        detail::file_lock lock_;
    };

    std::filesystem::path path_;
    // Shared so the store stays copyable; copies cancel together.
    std::shared_ptr<std::atomic<bool>> lock_waits_cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace moonbase
