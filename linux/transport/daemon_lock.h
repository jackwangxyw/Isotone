// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// One daemon at a time.
//
// Two daemons would both create a sink of the same name and both try to follow
// the default sink, and what the UI then reads out of the regions is whichever
// of them wrote last. Nothing stopped that: the `.deb` was safe only because
// systemd will not start a unit twice, and the Flatpak had no daemon of its own
// to start at all (decisions.md, "The Flatpak's daemon").
//
// The lock is an advisory exclusive flock on an object in the same POSIX shared
// memory the regions live in, so a daemon inside a Flatpak and one on the host
// contend for the same lock: /dev/shm is the host's either way, which is what
// --device=shm buys. The kernel drops it when the process dies, however it
// dies, so a daemon that is killed leaves nothing behind to clean up.

#pragma once

namespace isotone::transport {

// Held for as long as the object lives. Not copyable: the lock is the fd.
class DaemonLock {
public:
    DaemonLock() = default;
    ~DaemonLock();
    DaemonLock(const DaemonLock&) = delete;
    DaemonLock& operator=(const DaemonLock&) = delete;

    // True when this process now holds it. False when another already does, or
    // when the shared memory could not be opened at all, which `error` tells
    // apart: EWOULDBLOCK for the first, any other errno for the second.
    bool acquire(int* error = nullptr);

    bool held() const { return fd_ >= 0; }

    // Gives the lock up early. The destructor does this too.
    void release();

private:
    int fd_ = -1;
};

}  // namespace isotone::transport
