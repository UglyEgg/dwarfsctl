// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace dwarfsctl {

class OperationFileLock {
public:
    OperationFileLock() = default;
    ~OperationFileLock() { unlock(); }
    OperationFileLock(const OperationFileLock &) = delete;
    OperationFileLock &operator=(const OperationFileLock &) = delete;

    // The inode stays in place across operations. Unlinking it would let a
    // second process lock a new inode while a waiter still holds the old one.
    int acquire(const char *path, std::chrono::milliseconds timeout = {})
    {
        fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd < 0) return errno;
        struct stat info {};
        if (::fstat(fd, &info) != 0) return release(errno);
        if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid()) return release(EINVAL);

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            if (error == EINTR) continue;
            if (error != EWOULDBLOCK || std::chrono::steady_clock::now() >= deadline)
                return release(error);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 0;
    }

    void unlock()
    {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

private:
    int release(int error)
    {
        unlock();
        return error;
    }
    int fd = -1;
};

} // namespace dwarfsctl
