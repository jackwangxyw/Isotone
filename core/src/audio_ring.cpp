// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/audio_ring.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace isotone {

namespace {

bool is_power_of_two(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

}  // namespace

void audio_ring_init(AudioRingHeader* ring, uint32_t capacity) {
    if (ring == nullptr) {
        return;
    }
    std::memset(ring, 0, sizeof(AudioRingHeader));
    ring->capacity = capacity;
}

void AudioRingWriter::attach(AudioRingHeader* ring, uint32_t capacity) {
    ring_     = is_power_of_two(capacity) ? ring : nullptr;
    samples_  = ring_ != nullptr ? reinterpret_cast<float*>(ring_ + 1) : nullptr;
    capacity_ = capacity;
    next_     = 0;
    token_    = 0;
    owner_    = false;
}

void AudioRingWriter::set_channels(uint32_t channels) {
    channels_ = std::min(channels, kMaxChannels);
    if (owner_) {
        publish_channels();
    }
}

bool AudioRingWriter::claim(uint64_t token) {
    if (ring_ == nullptr || token == 0) {
        return false;
    }
    if (owner_) {
        return true;
    }
    std::atomic_ref<uint64_t> writer(ring_->writer);
    uint64_t current = writer.load(std::memory_order_relaxed);
    const bool available = current == 0 || (current >> 32) != (token >> 32);
    if (!available || !writer.compare_exchange_strong(current, token)) {
        return false;
    }
    token_ = token;
    owner_ = true;
    publish_channels();
    return true;
}

void AudioRingWriter::release() {
    if (!owner_) {
        return;
    }
    std::atomic_ref<uint64_t> writer(ring_->writer);
    uint64_t expected = token_;
    writer.compare_exchange_strong(expected, 0);
    owner_ = false;
}

// Restarts the ring under a new layout. The epoch goes odd for the duration, so
// a reader either sees the old layout throughout its copy or discards it.
void AudioRingWriter::publish_channels() {
    std::atomic_ref<uint32_t> epoch(ring_->epoch);
    const uint32_t odd = epoch.load(std::memory_order_relaxed) | 1u;
    epoch.store(odd, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    next_ = 0;
    std::atomic_ref<uint32_t>(ring_->channels).store(channels_, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(ring_->pending_index).store(0, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(ring_->write_index).store(0, std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_release);
    epoch.store(odd + 1, std::memory_order_relaxed);
}

void AudioRingWriter::write(const float* interleaved, uint32_t stride, uint32_t frames) {
    if (!owner_ || frames == 0 || channels_ == 0) {
        return;
    }
    const uint32_t end = next_ + frames;
    std::atomic_ref<uint32_t>(ring_->pending_index).store(end, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    // A call longer than the ring only has room for its last `capacity` frames.
    const uint32_t first  = frames > capacity_ ? frames - capacity_ : 0;
    const uint32_t stored = interleaved != nullptr ? std::min(stride, channels_) : 0;
    const uint32_t mask   = capacity_ - 1;
    for (uint32_t i = first; i < frames; ++i) {
        float* dst = samples_ + static_cast<size_t>((next_ + i) & mask) * channels_;
        uint32_t c = 0;
        for (; c < stored; ++c) {
            dst[c] = interleaved[static_cast<size_t>(i) * stride + c];
        }
        for (; c < channels_; ++c) {
            dst[c] = 0.0f;
        }
    }

    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_ref<uint32_t>(ring_->write_index).store(end, std::memory_order_relaxed);
    next_ = end;
}

uint32_t audio_ring_read(const AudioRingHeader* ring, AudioRingCursor* cursor, float* out,
                         uint32_t max_frames, uint32_t* channels) {
    if (ring == nullptr || cursor == nullptr || out == nullptr || channels == nullptr) {
        return 0;
    }
    std::atomic_ref<const uint32_t> epoch(ring->epoch);
    const uint32_t e1 = epoch.load(std::memory_order_relaxed);
    if ((e1 & 1u) != 0) {
        return 0;   // the writer is changing the layout
    }
    std::atomic_thread_fence(std::memory_order_acquire);

    const uint32_t capacity = std::atomic_ref<const uint32_t>(ring->capacity).load(std::memory_order_relaxed);
    const uint32_t ch = std::atomic_ref<const uint32_t>(ring->channels).load(std::memory_order_relaxed);
    const uint32_t end = std::atomic_ref<const uint32_t>(ring->write_index).load(std::memory_order_relaxed);
    if (!is_power_of_two(capacity) || ch == 0 || ch > kMaxChannels) {
        return 0;
    }
    if (cursor->epoch != e1) {
        cursor->epoch = e1;
        cursor->next = end;
        return 0;
    }

    // Unsigned differences stay correct when the 32-bit frame counter wraps.
    uint32_t start = cursor->next;
    uint32_t count = end - start;
    if (count > capacity) {
        start = end - capacity;
        count = capacity;
    }
    if (count > max_frames) {
        start = end - max_frames;
        count = max_frames;
    }

    const float* samples = audio_ring_samples(ring);
    const uint32_t pos = start & (capacity - 1);
    const uint32_t head = std::min(count, capacity - pos);
    std::memcpy(out, samples + static_cast<size_t>(pos) * ch,
                static_cast<size_t>(head) * ch * sizeof(float));
    std::memcpy(out + static_cast<size_t>(head) * ch, samples,
                static_cast<size_t>(count - head) * ch * sizeof(float));

    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t pending =
        std::atomic_ref<const uint32_t>(ring->pending_index).load(std::memory_order_relaxed);
    if (epoch.load(std::memory_order_relaxed) != e1) {
        cursor->epoch = 1;   // layout changed under the copy; resync next time
        return 0;
    }

    // The slot of frame i is reused by frame i + capacity. Anything more than
    // `capacity` behind the furthest write in progress may have been
    // overwritten while it was being copied.
    const uint32_t behind = pending - start;
    const uint32_t lost = behind > capacity ? std::min(count, behind - capacity) : 0;
    if (lost > 0) {
        std::memmove(out, out + static_cast<size_t>(lost) * ch,
                     static_cast<size_t>(count - lost) * ch * sizeof(float));
        count -= lost;
    }

    cursor->next = end;
    *channels = ch;
    return count;
}

}  // namespace isotone
