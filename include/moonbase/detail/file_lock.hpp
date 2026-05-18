#pragma once

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "moonbase/errors.hpp"

#if defined(_WIN32)
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// store.hpp pulls this header in via lock_for_update(), so any consumer of
// the public moonbase store API transitively sees <windows.h>. Without
// NOMINMAX, Win32 would inject min/max macros and clobber std::min/std::max
// in their translation units.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
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
// Windows: LockFileEx(LOCKFILE_EXCLUSIVE_LOCK) on a single-byte range —
// the range is required by the API but otherwise arbitrary; all callers
// using this class lock the same range.
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
        handle_ = ::CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw storage_error(
                "Could not open license lock file (error "
                + std::to_string(::GetLastError()) + ")");
        }

        OVERLAPPED overlapped{};
        if (!::LockFileEx(
                handle_,
                LOCKFILE_EXCLUSIVE_LOCK,
                0,
                lock_range_low_,
                lock_range_high_,
                &overlapped)) {
            const auto err = ::GetLastError();
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw storage_error(
                "Could not acquire license file lock (error "
                + std::to_string(err) + ")");
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
#if defined(_WIN32)
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
    }

    file_lock& operator=(file_lock&& other) noexcept
    {
        if (this != &other) {
            release();
#if defined(_WIN32)
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
#else
            fd_ = other.fd_;
            other.fd_ = -1;
#endif
        }
        return *this;
    }

    ~file_lock() { release(); }

private:
    void release() noexcept
    {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            ::UnlockFileEx(handle_, 0, lock_range_low_, lock_range_high_, &overlapped);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
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
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    static constexpr DWORD lock_range_low_ = 1;
    static constexpr DWORD lock_range_high_ = 0;
#else
    int fd_ = -1;
#endif
};

} // namespace moonbase::detail
