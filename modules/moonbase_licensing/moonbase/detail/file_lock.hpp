#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "moonbase/errors.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/locking.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace moonbase::detail {

// RAII exclusive lock on a regular file. Callers pass a dedicated sidecar
// path (e.g. "<license>.lock") — the file is created on first acquire and
// is not assumed to contain anything, so it can outlive any data file the
// caller may later unlink. That separation is what makes the lock safe
// against delete_local_license() on POSIX, where flocking the data file
// would orphan the lock on the unlinked inode.
//
// POSIX: flock(LOCK_EX) on the file's descriptor. Locks are per-open-file
// description and cooperative; only callers that go through this class
// observe each other.
// Windows: CRT _locking on a single-byte range. The range is
// required by the API but otherwise arbitrary; all callers using this class
// lock the same range. _locking accepts ranges past EOF, so we deliberately
// do not _chsize_s the file — SetEndOfFile on a sibling handle would race
// against the byte lock held by an earlier acquirer and surface as EACCES.
// This intentionally avoids <windows.h> in public SDK headers because
// store.hpp includes this detail header.
//
// The lock is requested without blocking and retried every few milliseconds
// while someone else holds it, which can be for the length of their network
// check. A blocking request could not be called off, so a plugin closing
// while it waited would hold up the host until that check finished. Setting
// `give_up` ends the wait instead, with storage_error. No time limit
// otherwise: the holder lets go once its own request returns or times out.
//
// Throws moonbase::storage_error on unrecoverable open/lock failures, and
// when it gives up waiting.
class file_lock {
public:
    file_lock(const std::filesystem::path& path, const std::atomic<bool>& give_up)
    {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            // Directory creation failure is non-fatal here: if the parent
            // can't be created the subsequent open will fail with a clearer
            // error.
        }

#if defined(_WIN32)
        auto fd = -1;
        const auto open_error = ::_wsopen_s(
            &fd,
            path.wstring().c_str(),
            _O_RDWR | _O_CREAT | _O_BINARY,
            _SH_DENYNO,
            _S_IREAD | _S_IWRITE);
        if (open_error != 0) {
            throw storage_error(
                "Could not open license lock file: "
                + std::generic_category().message(open_error));
        }
        fd_ = fd;
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            const auto err = errno;
            throw storage_error(
                "Could not open license lock file: "
                + std::generic_category().message(err));
        }
#endif

        auto pause = std::chrono::milliseconds(1);
        while (!try_lock()) {
            if (give_up.load(std::memory_order_acquire)) {
                release();
                throw storage_error("Stopped waiting for the license file lock");
            }
            std::this_thread::sleep_for(pause);
            pause = std::min(pause * 2, max_pause_);
        }
    }

    file_lock(const file_lock&) = delete;
    file_lock& operator=(const file_lock&) = delete;

    file_lock(file_lock&& other) noexcept
    {
        fd_ = other.fd_;
        other.fd_ = -1;
    }

    file_lock& operator=(file_lock&& other) noexcept
    {
        if (this != &other) {
            release();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~file_lock() { release(); }

private:
    // True once the lock is ours, false while another holder has it. Closes the
    // file and throws for any other failure.
    bool try_lock()
    {
#if defined(_WIN32)
        if (::_locking(fd_, _LK_NBLCK, lock_range_size_) == 0) {
            return true;
        }
        const auto err = errno;
        if (err == EACCES) {
            return false;
        }
#else
        if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            return true;
        }
        const auto err = errno;
        if (err == EWOULDBLOCK || err == EINTR) {
            return false;
        }
#endif
        release();
        throw storage_error(
            "Could not acquire license file lock: "
            + std::generic_category().message(err));
    }

    void release() noexcept
    {
#if defined(_WIN32)
        if (fd_ >= 0) {
            ::_lseek(fd_, 0, SEEK_SET);
            ::_locking(fd_, _LK_UNLCK, lock_range_size_);
            ::_close(fd_);
            fd_ = -1;
        }
#else
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

#if defined(_WIN32)
    static constexpr long lock_range_size_ = 1;
#endif
    static constexpr std::chrono::milliseconds max_pause_{20};
    int fd_ = -1;
};

} // namespace moonbase::detail
