// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The post-EQ audio ring (plan 4.7): the host writes the samples it just
// produced, the UI drains them for the spectrum. Single writer, single reader,
// no locks, and neither side ever waits for the other.
//
// The writer never reads a layout field back out of shared memory. Any
// authenticated user can write to the mapping, so a capacity or channel count
// taken from it could steer the host's writes outside the buffer. The writer
// keeps private copies instead, and the reader validates everything it reads.

#pragma once

#include <cstdint>

#include "isotone/types.h"

namespace isotone {

// Samples follow the header immediately, interleaved, `channels` per frame.
// Storage for capacity * kMaxChannels samples is reserved.
struct AudioRingHeader {
    uint32_t write_index;    // frames written; published after the samples are in place
    uint32_t pending_index;  // the write_index a write in progress will publish; published first
    uint32_t capacity;       // frames, a power of two, fixed when the region is created
    uint32_t channels;       // samples per frame currently in the ring
    uint32_t epoch;          // odd while the writer changes `channels`; readers resync when it moves
    uint32_t reserved;
    uint64_t writer;         // election token of the instance allowed to write, 0 = unclaimed
};
static_assert(sizeof(AudioRingHeader) == 32, "ring header layout must stay fixed");

inline const float* audio_ring_samples(const AudioRingHeader* ring) {
    return reinterpret_cast<const float*>(ring + 1);
}

// Called once by whichever side creates the region.
void audio_ring_init(AudioRingHeader* ring, uint32_t capacity);

// Writer side. Only one instance per ring writes; the rest stay quiet, because
// the spectrum needs exactly one source (plan 5.3).
class AudioRingWriter {
public:
    // `capacity` is the value the region was created with, never re-read from
    // the shared header.
    void attach(AudioRingHeader* ring, uint32_t capacity);

    // Records the frame layout. Published to readers only while this writer
    // owns the ring. Clamped to kMaxChannels.
    void set_channels(uint32_t channels);

    // Tries to become the ring's writer. Succeeds if nobody holds it, or if the
    // holder's token names a different process: an owner from another process
    // means the audio engine restarted while the UI kept the mapping open, and
    // that owner is dead. Real-time safe: one compare-and-swap.
    // Token layout: process id in the high 32 bits, an instance serial below.
    bool claim(uint64_t token);
    void release();
    bool owns() const { return owner_; }

    // Appends `frames` frames. `interleaved` holds `stride` samples per frame
    // and the first min(stride, channels) of each are stored; nullptr stores
    // silence. Does nothing unless this writer owns the ring.
    void write(const float* interleaved, uint32_t stride, uint32_t frames);

private:
    void publish_channels();

    AudioRingHeader* ring_     = nullptr;
    float*           samples_  = nullptr;
    uint32_t         capacity_ = 0;
    uint32_t         channels_ = 0;
    uint32_t         next_     = 0;
    uint64_t         token_    = 0;
    bool             owner_    = false;
};

// Reader side. A fresh cursor, or one whose epoch has moved on, synchronises to
// the newest frame and returns nothing; after that each call returns the frames
// written since the previous call.
struct AudioRingCursor {
    uint32_t epoch = 1;   // odd never matches a stable epoch, so the first read syncs
    uint32_t next  = 0;
};

// Copies up to `max_frames` unread frames into `out`, which must hold
// max_frames * kMaxChannels samples, and returns how many frames were copied.
// If more are waiting than fit, the newest are kept. Frames the writer
// overwrote before the reader got to them are dropped rather than returned
// torn. `channels` receives the samples per frame of what was copied.
uint32_t audio_ring_read(const AudioRingHeader* ring, AudioRingCursor* cursor, float* out,
                         uint32_t max_frames, uint32_t* channels);

}  // namespace isotone
