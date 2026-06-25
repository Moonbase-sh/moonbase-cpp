#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
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
// Windows: CRT _locking(_LK_LOCK) on a single-byte range. The range is
// required by the API but otherwise arbitrary; all callers using this class
// lock the same range. _locking accepts ranges past EOF, so we deliberately
// do not _chsize_s the file — SetEndOfFile on a sibling handle would race
// against the byte lock held by an earlier acquirer and surface as EACCES.
// This intentionally avoids <windows.h> in public SDK headers because
// store.hpp includes this detail header.
//
// Throws moonbase::storage_error on unrecoverable open/lock failures.
class file_lock {
public:
    explicit file_lock(const std::filesystem::path& path)
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
                + std::string(std::strerror(open_error)));
        }
        fd_ = fd;

        if (::_locking(fd_, _LK_LOCK, lock_range_size_) != 0) {
            const auto err = errno;
            ::_close(fd_);
            fd_ = -1;
            throw storage_error(
                "Could not acquire license file lock: "
                + std::string(std::strerror(err)));
        }
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            throw storage_error(
                std::string("Could not open license lock file: ")
                + std::strerror(errno));
        }

        if (::flock(fd_, LOCK_EX) != 0) {
            const auto err = errno;
            ::close(fd_);
            fd_ = -1;
            throw storage_error(
                std::string("Could not acquire license file lock: ")
                + std::strerror(err));
        }
#endif
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
    int fd_ = -1;
};

} // namespace moonbase::detail
