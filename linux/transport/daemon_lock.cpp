// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "daemon_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

namespace isotone::transport {

namespace {

// Beside the regions, which are "/isotone.<sink>" (shared_region.cpp). A name
// no sink key can collide with, because a key never contains a space.
constexpr char kLockName[] = "/isotone. daemon";

}  // namespace

bool DaemonLock::acquire(int* error) {
    if (fd_ >= 0) return true;
    // 0600: the lock is the user's, as the regions are.
    const int fd = ::shm_open(kLockName, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        if (error != nullptr) *error = errno;
        return false;
    }
    // Not O_CLOEXEC: the daemon may re-exec, and a lock that survived would then
    // be held against itself. Closing on exec is what we want.
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) *error = saved;
        return false;
    }
    fd_ = fd;
    if (error != nullptr) *error = 0;
    return true;
}

void DaemonLock::release() {
    if (fd_ < 0) return;
    // close() drops the flock with the open file description. The object is
    // deliberately left in place: unlinking it would let a daemon starting at
    // the same moment create a second one and lock that instead.
    ::close(fd_);
    fd_ = -1;
}

DaemonLock::~DaemonLock() { release(); }

}  // namespace isotone::transport
