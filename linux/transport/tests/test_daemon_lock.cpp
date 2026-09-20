// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// One daemon at a time: the lock two daemons would contend for.

#include <cerrno>

#include "daemon_lock.h"
#include "doctest.h"

using isotone::transport::DaemonLock;

TEST_CASE("the second holder is refused, and told which kind of refusal it is") {
    DaemonLock first;
    int error = -1;
    REQUIRE(first.acquire(&error));
    CHECK(error == 0);
    CHECK(first.held());

    // flock belongs to the open file description, not to the process, so a
    // second DaemonLock contends even from here. That is what makes this
    // testable without starting two daemons.
    DaemonLock second;
    int refused = -1;
    CHECK_FALSE(second.acquire(&refused));
    CHECK_FALSE(second.held());
    // EWOULDBLOCK is "someone else has it", which the daemon treats as success
    // and leaves with 0. Anything else is a real failure and must not be
    // mistaken for one.
    CHECK((refused == EWOULDBLOCK || refused == EAGAIN));
}

TEST_CASE("giving it up lets the next one have it") {
    DaemonLock first;
    REQUIRE(first.acquire());

    DaemonLock second;
    CHECK_FALSE(second.acquire());

    // A daemon that exits drops it; this is the same thing without the exit.
    first.release();
    CHECK_FALSE(first.held());
    CHECK(second.acquire());
    CHECK(second.held());
}

TEST_CASE("acquiring twice on the same lock is not a refusal") {
    // The daemon takes it once, but a caller that asks again must not be told
    // another daemon is running.
    DaemonLock lock;
    REQUIRE(lock.acquire());
    int error = -1;
    CHECK(lock.acquire(&error));
    CHECK(lock.held());
}

TEST_CASE("a released lock can be taken again by the same object") {
    DaemonLock lock;
    REQUIRE(lock.acquire());
    lock.release();
    CHECK_FALSE(lock.held());
    CHECK(lock.acquire());
    CHECK(lock.held());
}
