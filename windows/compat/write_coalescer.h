// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Rate limiting for live edits on the compatibility backend (plan 5.5).
//
// Every write to Isotone.txt makes Equalizer APO reload and crossfade for
// 10 ms, and writes faster than that queue behind its load semaphore, adding
// latency without adding smoothness. So a drag is coalesced to about 30 writes
// a second: the first change goes out at once, later ones inside the interval
// replace each other, and the last one always lands.
//
// Single-threaded and driven by the caller: submit() on every edit, poll() from
// a timer (time_until_due() says when). The clock is injected so the behaviour
// is tested exactly rather than by sleeping.

#pragma once

#include <windows.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace isotone::compat {

inline constexpr std::chrono::milliseconds kCompatWriteInterval{33};

class WriteCoalescer {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Sink = std::function<DWORD(const std::string&)>;

    WriteCoalescer(Sink sink, std::chrono::milliseconds interval = kCompatWriteInterval,
                   Clock clock = nullptr);

    // Offers new content. Written now if the interval since the last write has
    // passed, otherwise held as the pending write, replacing older pending
    // content. Content identical to what was last written is not written again.
    DWORD submit(std::string content);

    // Writes the pending content if it is due. A failed write stays pending.
    DWORD poll();

    // Writes the pending content now, ignoring the interval: the end of a drag,
    // or a persist.
    DWORD flush();

    bool has_pending() const { return pending_.has_value(); }
    std::chrono::milliseconds time_until_due() const;
    size_t writes() const { return writes_; }

private:
    DWORD write(std::string content);

    Sink sink_;
    std::chrono::milliseconds interval_;
    Clock clock_;
    std::optional<std::string> pending_;
    std::optional<std::string> last_written_;
    std::chrono::steady_clock::time_point last_write_time_{};
    size_t writes_ = 0;
};

}  // namespace isotone::compat
