// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "write_coalescer.h"

namespace isotone::compat {

WriteCoalescer::WriteCoalescer(Sink sink, std::chrono::milliseconds interval, Clock clock)
    : sink_(std::move(sink)),
      interval_(interval),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })) {}

DWORD WriteCoalescer::write(std::string content) {
    if (last_written_ && *last_written_ == content) {
        pending_.reset();
        return ERROR_SUCCESS;
    }
    const DWORD e = sink_(content);
    if (e != ERROR_SUCCESS) {
        pending_ = std::move(content);
        return e;
    }
    last_written_ = std::move(content);
    last_write_time_ = clock_();
    pending_.reset();
    ++writes_;
    return ERROR_SUCCESS;
}

DWORD WriteCoalescer::submit(std::string content) {
    if (writes_ == 0 || clock_() - last_write_time_ >= interval_) {
        return write(std::move(content));
    }
    pending_ = std::move(content);
    return ERROR_SUCCESS;
}

DWORD WriteCoalescer::poll() {
    if (!pending_ || (writes_ != 0 && clock_() - last_write_time_ < interval_)) {
        return ERROR_SUCCESS;
    }
    return write(std::move(*pending_));
}

DWORD WriteCoalescer::flush() {
    return pending_ ? write(std::move(*pending_)) : ERROR_SUCCESS;
}

std::chrono::milliseconds WriteCoalescer::time_until_due() const {
    if (!pending_ || writes_ == 0) return std::chrono::milliseconds{0};
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock_() - last_write_time_);
    return elapsed >= interval_ ? std::chrono::milliseconds{0} : interval_ - elapsed;
}

}  // namespace isotone::compat
